#include "StaticInstancing.h"

#include "Buffer.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/UI.h"

#include <RE/N/NiSkinPartition.h>
#include <imgui.h>

#include <chrono>
#include <atomic>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	StaticInstancing::Settings,
	Enabled,
	IncludeAlphaTested,
	IncludeDepthPrepass,
	IncludeMeshLOD,
	IncludeTrees,
	InstanceSingles,
	MinBatchSize,
	MinCasterRadius,
	MinCasterRadiusFocus,
	AuditCounters,
	IncludeLighting,
	MaxInstancesPerFrame)

namespace
{
	// Must match kStaticInstancingMagic in Utility.hlsl. A vanilla draw that reads a stale b13
	// sees some other value and takes the normal path.
	constexpr uint32_t kMagic = 0x5A17C0DE;
	constexpr uint32_t kInstanceStride = 128;  // world rows, TreeParams, previous world rows, MaterialData
	// Descriptor bit that selects the instanced Lighting permutation (STATIC_INSTANCING_LIGHTING) and,
	// on Utility descriptors, the instanced depth pre-pass permutation (STATIC_INSTANCING_DEPTH).
	constexpr uint32_t kLightingInstancedBit = 0x80000000u;
	constexpr uint32_t kPrepassInstancedBit = 0x80000000u;
	constexpr uint32_t kBatchCBSlot = 13;  // b13: unused by the game and by every other feature (b8 is Light Limit Fix)
	constexpr uint32_t kInstanceSRVSlot = 9;
	constexpr uint32_t kUtilityTechniqueBase = 0x2B;
	constexpr uint32_t kBlockSize = 64;  // batch bases are aligned to this many instances
	// NDC depth bias applied to every pre-pass draw while the pre-pass option is active. Four
	// float ulps at z = 1, comfortably above any difference the compiler can introduce, and far
	// below the depth buffer's own resolution at any distance that matters.
	constexpr float kPrepassDepthBias = 4.76837158e-7f;
	// BSGraphics depth-stencil state table: [depthMode][40 stencil modes], bound by SetDirtyStates
	// (1.6.1170 decompile: (stencilMode + depthMode * 0x28) * 8 + table). AE only for now.
	constexpr uint32_t kDepthStencilTableIdAE = 411362;
	constexpr uint32_t kStencilModes = 40;

	using UF = SIE::ShaderCache::UtilityShaderFlags;


	// Anything that changes what the vertex shader does with World, or that is not a plain
	// shadow-map pass. RenderDepth is excluded because Terrain Blending sequences those passes.
	constexpr uint32_t kExcludedBits =
		static_cast<uint32_t>(UF::Skinned) |
		static_cast<uint32_t>(UF::LodLandscape) |
		static_cast<uint32_t>(UF::RenderNormal) |
		static_cast<uint32_t>(UF::RenderNormalClear) |
		static_cast<uint32_t>(UF::RenderShadowmapPb) |
		static_cast<uint32_t>(UF::RenderShadowmask) |
		static_cast<uint32_t>(UF::RenderShadowmaskSpot) |
		static_cast<uint32_t>(UF::RenderShadowmaskPb) |
		static_cast<uint32_t>(UF::RenderShadowmaskDpb) |
		static_cast<uint32_t>(UF::RenderBaseTexture) |
		static_cast<uint32_t>(UF::DepthWriteDecals) |
		static_cast<uint32_t>(UF::OpaqueEffect) |
		static_cast<uint32_t>(UF::LocalMapFogOfWar);

	template <class T>
	void SafeRelease(T*& a_ptr)
	{
		if (a_ptr) {
			a_ptr->Release();
			a_ptr = nullptr;
		}
	}
}

bool StaticInstancing::batchCBReady() const
{
	return enabledCB != nullptr && disabledCB != nullptr && globals::d3d::context != nullptr;
}

size_t StaticInstancing::BatchKeyHash::operator()(const BatchKey& k) const noexcept
{
	size_t h = std::hash<const void*>{}(k.rendererData);
	auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
	mix(std::hash<const void*>{}(k.material));
	mix(std::hash<uint64_t>{}(k.propertyFlags));
	mix(static_cast<size_t>(k.passState) * 0x27D4EB2FULL);
	mix(static_cast<size_t>(k.technique) * 0x9E3779B1ULL);
	mix(static_cast<size_t>(k.renderFlags) * 0x85EBCA77ULL);
	mix(static_cast<size_t>(k.alpha) * 0xC2B2AE3DULL);
	return h;
}

std::pair<std::string, std::vector<std::string>> StaticInstancing::GetFeatureSummary()
{
	return { "Draws identical static shadow casters once with hardware instancing instead of once per object, cutting render-thread draw submission in shadow-map passes.",
		{ "Shadow-map passes only; depth pre-pass, skinned, tree-animated and landscape meshes are untouched.",
			"With coalescing off, still reports how many draws would collapse.",
			"Alpha-tested casters are opt-in." } };
}

void StaticInstancing::SetupResources()
{
	ReleaseResources();

	auto* device = globals::d3d::device;
	if (!device)
		return;

	D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
	if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))))
		noOverwriteSupported = options.MapNoOverwriteOnDynamicBufferSRV != 0;
	else
		noOverwriteSupported = false;

	ringCapacity = std::max<uint32_t>(settings.MaxInstancesPerFrame, 4096);
	ringCapacity = (ringCapacity + kBlockSize - 1) / kBlockSize * kBlockSize;
	deferred.reserve(4096);
	buckets.reserve(2048);
	bucketLookup.reserve(4096);
	measure.reserve(4096);

	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = ringCapacity * kInstanceStride;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = kInstanceStride;
		if (FAILED(device->CreateBuffer(&desc, nullptr, &instanceBuffer))) {
			logger::error("[Static Instancing] instance buffer creation failed");
			ReleaseResources();
			return;
		}
		Util::SetResourceName(instanceBuffer, "StaticInstancing::instanceBuffer");

		const uint32_t blockCount = ringCapacity / kBlockSize;
		instanceSRVs.reserve(blockCount);
		for (uint32_t block = 0; block < blockCount; ++block) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = block * kBlockSize;
			srvDesc.Buffer.NumElements = ringCapacity - block * kBlockSize;
			ID3D11ShaderResourceView* srv = nullptr;
			if (FAILED(device->CreateShaderResourceView(instanceBuffer, &srvDesc, &srv))) {
				logger::error("[Static Instancing] instance SRV creation failed at block {}", block);
				ReleaseResources();
				return;
			}
			instanceSRVs.push_back(srv);
		}
	}

	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 16;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		// Base is always 0: the bound SRV already starts at the batch's first instance. Pad.x carries
		// the pre-pass depth bias (as float bits); it stays zero until the main pass depth states have
		// been swapped to less-or-equal (ArmPrepassBias), so a failed swap can never bias anything.
		const uint32_t biasBits = 0;
		const uint32_t enabled[4] = { 0, kMagic, biasBits, 0 };
		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = enabled;
		if (FAILED(device->CreateBuffer(&desc, &init, &enabledCB))) {
			logger::error("[Static Instancing] enabled constant buffer creation failed");
			ReleaseResources();
			return;
		}
		Util::SetResourceName(enabledCB, "StaticInstancing::enabledCB");

		const uint32_t zeros[4] = { 0, 0, biasBits, 0 };
		init.pSysMem = zeros;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&desc, &init, &disabledCB))) {
			logger::error("[Static Instancing] disabled constant buffer creation failed");
			ReleaseResources();
			return;
		}
		Util::SetResourceName(disabledCB, "StaticInstancing::disabledCB");
	}

	ringUsed = 0;
	ringDiscardPending = true;
	biasArmed = false;
	depthStatesSwapped = false;
	swapAttempts = 0;
	SwapMainPassDepthStates(prepassActive);
	logger::info("[Static Instancing] resources ready: {} instances in {} blocks, NO_OVERWRITE on SRV buffers {}", ringCapacity, instanceSRVs.size(), noOverwriteSupported ? "supported" : "unsupported (discard per flush)");
}

void StaticInstancing::ArmPrepassBias(bool a_arm)
{
	if (biasArmed == a_arm || !batchCBReady())
		return;
	auto* context = globals::d3d::context;
	uint32_t biasBits = 0;
	if (a_arm)
		std::memcpy(&biasBits, &kPrepassDepthBias, sizeof(biasBits));
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(context->Map(enabledCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		const uint32_t enabled[4] = { 0, kMagic, biasBits, 0 };
		std::memcpy(mapped.pData, enabled, sizeof(enabled));
		context->Unmap(enabledCB, 0);
	}
	if (SUCCEEDED(context->Map(disabledCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		const uint32_t zeros[4] = { 0, 0, biasBits, 0 };
		std::memcpy(mapped.pData, zeros, sizeof(zeros));
		context->Unmap(disabledCB, 0);
	}
	biasArmed = a_arm;
	logger::info("[Static Instancing] pre-pass depth bias {}", a_arm ? "armed" : "disarmed");
}

void StaticInstancing::SwapMainPassDepthStates(bool a_enable)
{
	if (!a_enable) {
		for (auto& [slot, original] : swappedDepthStates)
			*slot = original;
		swappedDepthStates.clear();
		for (auto*& state : lessEqualStates)
			SafeRelease(state);
		lessEqualStates.clear();
		depthStatesSwapped = false;
		ArmPrepassBias(false);
		return;
	}
	if (!REL::Module::IsAE()) {
		logger::warn("[Static Instancing] pre-pass mode needs the AE depth-stencil table id; not enabled on this runtime");
		prepassActive = false;
		return;
	}
	auto* device = globals::d3d::device;
	auto** table = reinterpret_cast<ID3D11DepthStencilState**>(REL::ID(kDepthStencilTableIdAE).address());
	swapAttempts++;
	// The engine fills this table lazily, so this runs again each frame from Prepass until an
	// Equal state has been found. Every Equal state in the whole table is replaced, whichever row
	// it sits in; row and column are logged the first time for verification.
	uint32_t seen = 0;
	uint32_t equalFound = 0;
	uint32_t funcHistogram[8] = {};
	for (uint32_t index = 0; index < 7 * kStencilModes; ++index) {
		auto** slot = table + index;
		auto* original = *slot;
		if (!original)
			continue;
		seen++;
		bool alreadySwapped = false;
		for (auto& [s, o] : swappedDepthStates)
			if (s == slot) {
				alreadySwapped = true;
				break;
			}
		if (alreadySwapped)
			continue;
		D3D11_DEPTH_STENCIL_DESC desc{};
		original->GetDesc(&desc);
		if (swapAttempts == 1 && seen <= 12)
			logger::info("[Static Instancing] depth state [{}][{}]: enable {} write {} func {} stencil {}", index / kStencilModes, index % kStencilModes, desc.DepthEnable, static_cast<int>(desc.DepthWriteMask), static_cast<int>(desc.DepthFunc), desc.StencilEnable);
		funcHistogram[static_cast<int>(desc.DepthFunc) & 7]++;
		if (desc.DepthFunc != D3D11_COMPARISON_EQUAL)
			continue;
		equalFound++;
		desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		ID3D11DepthStencilState* replacement = nullptr;
		if (FAILED(device->CreateDepthStencilState(&desc, &replacement)) || !replacement)
			continue;
		Util::SetResourceName(replacement, "StaticInstancing::mainPassLessEqual");
		lessEqualStates.push_back(replacement);
		swappedDepthStates.emplace_back(slot, original);
		*slot = replacement;
	}
	if (equalFound > 0 || swapAttempts == 1 || swapAttempts % 600 == 0)
		logger::info("[Static Instancing] depth-state table: {} populated, {} Equal states replaced this pass, {} replaced in total (attempt {}); DepthFunc histogram never/less/equal/lessEq/greater/notEq/greaterEq/always = {} {} {} {} {} {} {} {}", seen, equalFound, swappedDepthStates.size(), swapAttempts, funcHistogram[1], funcHistogram[2], funcHistogram[3], funcHistogram[4], funcHistogram[5], funcHistogram[6], funcHistogram[7], funcHistogram[0]);
	if (!swappedDepthStates.empty()) {
		depthStatesSwapped = true;
		ArmPrepassBias(true);
	}
}

void StaticInstancing::ReleaseResources()
{
	SwapMainPassDepthStates(false);
	for (auto*& srv : instanceSRVs)
		SafeRelease(srv);
	instanceSRVs.clear();
	SafeRelease(instanceBuffer);
	SafeRelease(enabledCB);
	SafeRelease(disabledCB);
	ringCapacity = 0;
}

std::vector<std::pair<std::string_view, std::string_view>> StaticInstancing::GetShaderDefineOptions()
{
	if (prepassActive || settings.IncludeDepthPrepass)
		return { { "STATIC_INSTANCING_PREPASS", "" } };
	return {};
}

void StaticInstancing::Prepass()
{
	if (!prepassActive || !disabledCB)
		return;
	// The engine creates depth-stencil states on first use; keep scanning until the Equal states
	// exist and have been replaced (and a while after, in case more stencil variants appear).
	if (!depthStatesSwapped || swapAttempts < 900)
		SwapMainPassDepthStates(true);
	// Keep b13 bound with the bias for every pre-pass draw; instanced draws rebind their own.
	globals::d3d::context->VSSetConstantBuffers(kBatchCBSlot, 1, &disabledCB);
}

void StaticInstancing::Reset()
{
	// Nothing survives a frame anyway; drop whatever a partially rendered frame left behind.
	deferred.clear();
	buckets.clear();
	flushGeneration++;
	pending = {};
	batchDepth = 0;
	flushing = false;
}

void StaticInstancing::PostPostLoad()
{
	prepassActive = loaded && settings.IncludeDepthPrepass;
	if (loaded && !hooksInstalled) {
		Hooks::Install();
		hooksInstalled = true;
	}
}

void StaticInstancing::LoadSettings(json& o_json)
{
	settings = o_json;
}

void StaticInstancing::SaveSettings(json& o_json)
{
	o_json = settings;
}

void StaticInstancing::RestoreDefaultSettings()
{
	settings = {};
}

json StaticInstancing::GetStatsJson() const
{
	const auto& c = last;
	return json{
		{ "enabled", settings.Enabled },
		{ "includeAlphaTested", settings.IncludeAlphaTested },
		{ "instanceSingles", settings.InstanceSingles },
		{ "minBatchSize", settings.MinBatchSize },
		{ "ringCapacity", ringCapacity },
		{ "utilityPasses", c.utilityPasses },
		{ "eligible", c.eligible },
		{ "eligibleAlpha", c.eligibleAlpha },
		{ "uniqueKeys", c.uniqueKeys },
		{ "collapsible", c.collapsible },
		{ "deferred", c.deferred },
		{ "collapsed", c.collapsed },
		{ "instancedDraws", c.instancedDraws },
		{ "singlesReplayed", c.singlesReplayed },
		{ "ringOverflow", c.ringOverflow },
		{ "pendingDropped", c.pendingDropped },
		{ "depthToggles", c.depthToggles },
		{ "flushes", c.flushes },
		{ "maps", c.maps },
		{ "flushUs", c.flushUs },
		{ "writeUs", c.writeUs },
		{ "drawUs", c.drawUs },
		{ "excludedTechnique", c.excludedTechnique },
		{ "excludedGeometry", c.excludedGeometry },
		{ "excludedSkinned", c.excludedSkinned },
		{ "excludedProperty", c.excludedProperty },
		{ "excludedAlpha", c.excludedAlpha },
		{ "excludedFading", c.excludedFading },
		{ "culledSmallCasters", c.culledSmallCasters },
		{ "waterPasses", c.waterPasses },
		{ "effectPasses", c.effectPasses },
		{ "minCasterRadius", settings.MinCasterRadius },
		{ "descShadowmap", c.descShadowmap },
		{ "descDepth", c.descDepth },
		{ "descShadowmask", c.descShadowmask },
		{ "descTreeAnim", c.descTreeAnim },
		{ "descSkinned", c.descSkinned },
		{ "descLodLandscape", c.descLodLandscape },
		{ "descAlphaTest", c.descAlphaTest },
		{ "includeDepthPrepass", settings.IncludeDepthPrepass },
		{ "includeMeshLOD", settings.IncludeMeshLOD },
		{ "prepassActive", prepassActive },
		{ "prepassBiasArmed", biasArmed },
		{ "depthStatesSwapped", swappedDepthStates.size() },
		{ "includeTrees", settings.IncludeTrees },
		{ "excludedGeometryByType", std::vector<uint32_t>(std::begin(c.excludedGeometryByType), std::end(c.excludedGeometryByType)) },
		{ "lightingPasses", c.lightingPasses },
		{ "lightingStatic", c.lightingStatic },
		{ "lightingUniqueKeys", c.lightingUniqueKeys },
		{ "lightingCollapsible", c.lightingCollapsible },
		{ "lightingSkinned", c.lightingSkinned },
		{ "lightingOtherType", c.lightingOtherType },
		{ "lightingEligible", c.lightingEligible },
		{ "lightingDeferred", c.lightingDeferred },
		{ "lightingCollapsed", c.lightingCollapsed },
		{ "lightingInstancedDraws", c.lightingInstancedDraws },
		{ "lightingShaderMisses", c.lightingShaderMisses },
		{ "prepassSkipped", c.prepassSkipped },
		{ "prepassInstancedDraws", c.prepassInstancedDraws },
		{ "prepassShaderMisses", c.prepassShaderMisses },
		{ "forcedDepthWrite", c.forcedDepthWrite },
		{ "includeLighting", settings.IncludeLighting },
		{ "lastDescriptor", lastDescriptor },
	};
}

void StaticInstancing::DrawSettings()
{
	ImGui::TextWrapped("Draws identical static shadow casters once with hardware instancing. Turn coalescing off to measure how many draws would collapse without changing what is rendered.");

	ImGui::Checkbox("Coalesce draws", &settings.Enabled);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Off keeps the counters running but renders exactly like vanilla.");
	}
	ImGui::Checkbox("Include alpha-tested casters", &settings.IncludeAlphaTested);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Foliage, fences and other cut-out meshes. Their batch key includes the material, so only exact clones batch.");
	}
	ImGui::Checkbox("Include depth pre-pass (restart required)", &settings.IncludeDepthPrepass);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Also coalesces the main view's depth pre-pass. Pre-pass depths get a four-ulp bias and the main pass depth test becomes less-or-equal, so instanced and vanilla depths never have to match exactly. Takes effect after a restart.");
	}
	if (prepassActive != settings.IncludeDepthPrepass)
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Pre-pass setting changes after a restart.");
	ImGui::Checkbox("Include mesh-LOD shapes", &settings.IncludeMeshLOD);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("BSMeshLODTriShape geometry (statics with built-in LOD levels). Instances share the LOD level through the batch key.");
	}
	ImGui::Checkbox("Include wind-animated trees (shadow maps)", &settings.IncludeTrees);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Trees sway per instance through the instance record. Shadow-map passes only.");
	}
	ImGui::Checkbox("Include main pass (Lighting)", &settings.IncludeLighting);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Instances identical static meshes in the main pass. Instanced objects are skipped in the depth pre-pass and drawn test-and-write through a dedicated shader permutation. Converges one frame after an object appears.");
	}
	ImGui::Checkbox("Audit counters (Lighting duplicates, water, effects)", &settings.AuditCounters);
	ImGui::Checkbox("Instance singles (debug)", &settings.InstanceSingles);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Sends every eligible draw through the instanced vertex path with one instance. If shadows look identical to vanilla in this mode, the matrix path is correct.");
	}
	ImGui::SliderFloat("Cull shadow casters below radius", &settings.MinCasterRadius, 0.0f, 200.0f, "%.0f units");
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Static geometry with a world bound radius below this is not drawn into shadow maps at all. 0 disables. Actors are never culled.");
	}
	int minBatch = static_cast<int>(settings.MinBatchSize);
	if (ImGui::SliderInt("Minimum batch size", &minBatch, 2, 16))
		settings.MinBatchSize = static_cast<uint32_t>(minBatch);

	ImGui::Separator();
	ImGui::Text("Last frame");
	const auto& c = last;
	if (ImGui::BeginTable("StaticInstancingStats", 2, ImGuiTableFlags_SizingStretchProp)) {
		auto row = [](const char* label, uint32_t value) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%u", value);
		};
		row("Utility passes in batches", c.utilityPasses);
		row("Eligible", c.eligible);
		row("  of which alpha-tested", c.eligibleAlpha);
		row("Unique keys", c.uniqueKeys);
		row("Collapsible (eligible minus unique)", c.collapsible);
		row("Deferred", c.deferred);
		row("Collapsed (draws removed)", c.collapsed);
		row("Instanced draws issued", c.instancedDraws);
		row("Singles replayed", c.singlesReplayed);
		row("Ring overflow buckets", c.ringOverflow);
		row("Pending dropped", c.pendingDropped);
		row("Excluded: technique", c.excludedTechnique);
		row("Excluded: geometry type", c.excludedGeometry);
		row("Excluded: skinned", c.excludedSkinned);
		row("Excluded: property", c.excludedProperty);
		row("Excluded: alpha-tested (opt-in off)", c.excludedAlpha);
		row("Excluded: fading (stencil)", c.excludedFading);
		row("Culled small casters", c.culledSmallCasters);
		row("Passes with RenderShadowmap", c.descShadowmap);
		row("Passes with RenderDepth", c.descDepth);
		row("Passes with shadow mask", c.descShadowmask);
		row("Passes with TreeAnim", c.descTreeAnim);
		row("Passes with Skinned", c.descSkinned);
		row("Passes with LodLandscape", c.descLodLandscape);
		row("Passes with AlphaTest", c.descAlphaTest);
		row("Lighting passes", c.lightingPasses);
		row("Lighting static TriShapes", c.lightingStatic);
		row("Lighting unique keys", c.lightingUniqueKeys);
		row("Lighting collapsible", c.lightingCollapsible);
		row("Main pass: eligible", c.lightingEligible);
		row("Main pass: collapsed", c.lightingCollapsed);
		row("Main pass: instanced draws", c.lightingInstancedDraws);
		row("Main pass: shader misses", c.lightingShaderMisses);
		row("Pre-pass draws folded", c.prepassSkipped);
		row("Pre-pass instanced draws", c.prepassInstancedDraws);
		row("Pre-pass shader misses", c.prepassShaderMisses);
		row("Forced depth write", c.forcedDepthWrite);
		row("Depth mode toggles", c.depthToggles);
		row("Flushes / maps", c.flushes);
		row("Maps", c.maps);
		ImGui::Text("Flush %.0f us, write %.0f us, draw %.0f us", c.flushUs, c.writeUs, c.drawUs);
		ImGui::EndTable();
	}
	if (c.eligible > 0) {
		const float ratio = 100.0f * static_cast<float>(c.collapsible) / static_cast<float>(c.eligible);
		ImGui::Text("Collapse ratio: %.1f%% of eligible draws", ratio);
	}
}

void StaticInstancing::PruneStamps()
{
	const uint32_t oldest = frameStamp - 2;
	std::erase_if(geometryStamps, [&](const auto& kv) { return kv.second.candidate < oldest && kv.second.skipped < oldest && kv.second.instanced < oldest; });
	std::erase_if(lightingCandidateKeys, [&](const auto& kv) { return kv.second < oldest; });
	std::erase_if(measure, [&](const auto& kv) { return kv.second.stamp < oldest; });
	std::erase_if(lightingMeasure, [&](const auto& kv) { return kv.second.stamp < oldest; });
	std::erase_if(bucketLookup, [&](const auto& kv) { return kv.second.generation + 2 < flushGeneration; });
}

void StaticInstancing::BeginFrameIfNeeded()
{
	if (!frameChecker.IsNewFrame())
		return;

	last = frame;
	last.uniqueKeys = frame.uniqueKeys + measureTouched;
	last.lightingUniqueKeys = lightingMeasureTouched;
	frame = {};
	measureTouched = 0;
	lightingMeasureTouched = 0;
	// New stamp: last frame's instanced geometries are this frame's pre-pass candidates by stamp
	// arithmetic, plus the Lighting keys that reached MinBatchSize last frame.
	frameStamp++;
	candidateCount = instancedGeometryCount;
	instancedGeometryCount = 0;
	skippedCount = 0;
	for (auto& [geometry, key] : lightingKeyByGeometry) {
		auto it = lightingCandidateKeys.find(key);
		if (it != lightingCandidateKeys.end() && it->second + 1 == frameStamp)
			MarkCandidate(geometry);
	}
	lightingKeyByGeometry.clear();
	if ((frameStamp & 1023) == 0)
		PruneStamps();

	ringUsed = 0;
	ringDiscardPending = true;

	if (!deferred.empty()) {
		// A group never flushed (loop exited early). Drop it rather than replay into the wrong state.
		deferred.clear();
		buckets.clear();
		flushGeneration++;
	}
	pending = {};
}

void StaticInstancing::AuditLightingPass(RE::BSRenderPass* a_pass, uint32_t a_technique, uint32_t a_renderFlags)
{
	frame.lightingPasses++;
	auto* geometry = a_pass->geometry;
	if (!geometry || geometry->GetType().get() != RE::BSGeometry::Type::kTriShape) {
		frame.lightingOtherType++;
		return;
	}
	const auto& runtime = geometry->GetGeometryRuntimeData();
	if (runtime.skinInstance) {
		frame.lightingSkinned++;
		return;
	}
	auto* property = a_pass->shaderProperty;
	if (!property || !runtime.rendererData)
		return;
	frame.lightingStatic++;
	BatchKey key{};
	key.rendererData = runtime.rendererData;
	key.material = property->GetBaseMaterial();
	key.propertyFlags = static_cast<uint64_t>(property->flags.get());
	key.technique = a_technique;
	key.renderFlags = a_renderFlags;
	key.passState = static_cast<uint32_t>(a_pass->accumulationHint) | (static_cast<uint32_t>(*reinterpret_cast<const std::uint8_t*>(&a_pass->LODMode)) << 8) | (static_cast<uint32_t>(a_pass->numLights) << 16) | (static_cast<uint32_t>(a_pass->numShadowLights) << 24);
	auto& seen = lightingMeasure[key];
	if (seen.stamp != frameStamp) {
		seen.stamp = frameStamp;
		seen.count = 0;
		lightingMeasureTouched++;
	}
	if (seen.count > 0)
		frame.lightingCollapsible++;
	seen.count++;
}

void StaticInstancing::ComputeTreeParams(const RE::BSRenderPass* a_pass, float* o_params)
{
	// Same values and operation order as BSUtilityShader::SetupGeometry's TreeAnim block
	// (1.6.1170 decompile): x = leaf timer * 6, y = the shadow scene node's wind value, z = the
	// distance-dampened amplitude, w = leaf frequency. Defaults 0, wind, 1, 1 without a leaf node.
	RE::BSLeafAnimNode* leaf = nullptr;
	if (auto* property = a_pass->shaderProperty; property && property->fadeNode)
		leaf = property->fadeNode->AsLeafAnimNode();
	auto& shaderState = RE::BSShaderManager::State::GetSingleton();
	float wind = 0.0f;
	if (auto* ssn = shaderState.shadowSceneNode[0])
		wind = *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(ssn) + 0x304);
	float distance = 0.0f;
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float timer = 0.0f;
	if (leaf) {
		// BSLeafAnimNode keeps these private; read them at their documented offsets (SE 0x158.., VR 0x110..).
		const auto* leafBytes = reinterpret_cast<const std::uint8_t*>(leaf) + (REL::Module::IsVR() ? 0x110 : 0x158);
		const float sqrDistanceToCamera = *reinterpret_cast<const float*>(leafBytes + 0x0);
		const float leafAmplitudeRefr = *reinterpret_cast<const float*>(leafBytes + 0x4);
		const float leafFrequency = *reinterpret_cast<const float*>(leafBytes + 0x8);
		const float runningTime = *reinterpret_cast<const float*>(leafBytes + 0xC);
		timer = runningTime * 6.0f;
		// x * rsqrt(x) with one Newton step, exactly as the engine does it
		const float x = sqrDistanceToCamera;
		std::uint32_t bits;
		std::memcpy(&bits, &x, sizeof(bits));
		bits = 0x5f3759df - (static_cast<std::int32_t>(bits) >> 1);
		float y;
		std::memcpy(&y, &bits, sizeof(y));
		float t = x * 0.5f;
		t = t * y;
		t = t * y;
		float r = 1.5f - t;
		r = r * y;
		distance = r * x;
		amplitude = leafAmplitudeRefr;
		frequency = leafFrequency;
	}
	const float start = shaderState.leafAnimDampenDistStartSPU;
	const float end = shaderState.leafAnimDampenDistEndSPU;
	float dampened = (distance - start) / (end - start);
	dampened = (1.0f - dampened) * amplitude;
	dampened = std::max(dampened, 0.0f);
	dampened = std::min(dampened, amplitude);
	o_params[0] = timer;
	o_params[1] = wind;
	o_params[2] = dampened;
	o_params[3] = frequency;
}

void StaticInstancing::CallWithForcedDepthWrite(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	using Fn = Hooks::BSBatchRenderer_RenderPassImmediately;
	if (forcedDepthActive) {
		// A Lighting flush already forced the mode for every pass it replays.
		frame.forcedDepthWrite++;
		Fn::func(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}
	const bool force = settings.IncludeLighting && skippedCount > 0 && a_pass->shader &&
	                   a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting && IsSkippedThisFrame(a_pass->geometry);
	if (!force) {
		Fn::func(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}
	frame.forcedDepthWrite++;
	BeginForcedDepthWrite();
	Fn::func(a_pass, a_technique, a_alphaTest, a_renderFlags);
	EndForcedDepthWrite();
}

void StaticInstancing::BeginForcedDepthWrite()
{
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	forcedDepthSaved = shadowState.depthStencilDepthMode;
	shadowState.depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTestWrite;
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	forcedDepthActive = true;
	frame.depthToggles++;
}

void StaticInstancing::EndForcedDepthWrite()
{
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	shadowState.depthStencilDepthMode = forcedDepthSaved;
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	forcedDepthActive = false;
	frame.depthToggles++;
}

bool StaticInstancing::IsPrepassInstanceCandidate(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags, BatchKey& o_key)
{
	if (!a_pass->shader || a_pass->shader->shaderType.get() != RE::BSShader::Type::Utility || !a_pass->geometry)
		return false;
	const uint32_t descriptor = a_technique - kUtilityTechniqueBase;
	// Plain depth pre-pass techniques only: the instanced permutation exists for exactly these bits.
	constexpr uint32_t kAllowed = static_cast<uint32_t>(UF::Vc) | static_cast<uint32_t>(UF::Texture) | static_cast<uint32_t>(UF::Normals) |
	                              static_cast<uint32_t>(UF::BinormalTangent) | static_cast<uint32_t>(UF::AlphaTest) | static_cast<uint32_t>(UF::RenderDepth);
	if ((descriptor & static_cast<uint32_t>(UF::RenderDepth)) == 0 || (descriptor & ~kAllowed) != 0)
		return false;
	if (!IsPrepassCandidate(a_pass->geometry))
		return false;
	const auto& runtime = a_pass->geometry->GetGeometryRuntimeData();
	if (!runtime.rendererData || !runtime.rendererData->vertexBuffer || !runtime.rendererData->indexBuffer || runtime.skinInstance)
		return false;
	auto* property = a_pass->shaderProperty;
	if (!property)
		return false;
	o_key = {};
	o_key.rendererData = runtime.rendererData;
	o_key.material = property->GetBaseMaterial();
	o_key.propertyFlags = static_cast<uint64_t>(property->flags.get());
	o_key.technique = a_technique;
	o_key.renderFlags = a_renderFlags;
	o_key.alpha = (a_alphaTest ? 1u << 24 : 0u);
	if (auto* alphaProperty = runtime.alphaProperty.get())
		o_key.alpha |= static_cast<uint32_t>(alphaProperty->alphaThreshold) | (static_cast<uint32_t>(alphaProperty->alphaFlags) << 8);
	o_key.passState = 0x20000000u;
	return true;
}

StaticInstancing::Verdict StaticInstancing::ClassifyLighting(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags, BatchKey& o_key)
{
	auto* geometry = a_pass->geometry;
	if (!geometry)
		return Verdict::kLightingExcluded;
	const auto geometryType = geometry->GetType().get();
	if (geometryType != RE::BSGeometry::Type::kTriShape && (geometryType != RE::BSGeometry::Type::kMeshLODTriShape || !settings.IncludeMeshLOD))
		return Verdict::kLightingExcluded;
	const auto& runtime = geometry->GetGeometryRuntimeData();
	if (!runtime.rendererData || !runtime.rendererData->vertexBuffer || !runtime.rendererData->indexBuffer || runtime.skinInstance)
		return Verdict::kLightingExcluded;
	if (!REL::Module::IsVR() && (reinterpret_cast<const std::uint8_t*>(geometry)[0x109] & 0x8) != 0)
		return Verdict::kLightingExcluded;
	auto* property = a_pass->shaderProperty;
	if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
		return Verdict::kLightingExcluded;
	using F = RE::BSShaderProperty::EShaderPropertyFlag;
	if (property->flags.any(F::kMultiTextureLandscape, F::kProjectedUV, F::kTreeAnim, F::kSkinned, F::kDecal, F::kDynamicDecal, F::kLODLandscape, F::kLODObjects, F::kEyeReflect, F::kHairTint, F::kFaceGenRGBTint))
		return Verdict::kLightingExcluded;
	if (!property->flags.all(F::kZBufferWrite, F::kZBufferTest))
		return Verdict::kLightingExcluded;
	if (a_pass->accumulationHint == 10 || a_pass->numShadowLights != 0 || a_pass->LODMode.singleLevel)
		return Verdict::kLightingExcluded;
	if (auto* alphaProperty = runtime.alphaProperty.get(); alphaProperty && (alphaProperty->alphaFlags & 1) != 0)
		return Verdict::kLightingExcluded;  // alpha blending is order dependent
	o_key = {};
	o_key.rendererData = runtime.rendererData;
	o_key.material = property->GetBaseMaterial();
	o_key.propertyFlags = static_cast<uint64_t>(property->flags.get());
	o_key.technique = a_technique;
	o_key.renderFlags = a_renderFlags;
	o_key.alpha = (a_alphaTest ? 1u << 24 : 0u);
	if (auto* alphaProperty = runtime.alphaProperty.get())
		o_key.alpha |= static_cast<uint32_t>(alphaProperty->alphaThreshold) | (static_cast<uint32_t>(alphaProperty->alphaFlags) << 8);
	o_key.passState = 0x40000000u | static_cast<uint32_t>(a_pass->accumulationHint) | (static_cast<uint32_t>(*reinterpret_cast<const std::uint8_t*>(&a_pass->LODMode)) << 8) | (static_cast<uint32_t>(a_pass->numLights) << 16) | (static_cast<uint32_t>(a_pass->extraParam) << 24);
	return Verdict::kLightingEligible;
}

bool StaticInstancing::IsTerrainPass(const RE::BSRenderPass* a_pass)
{
	return a_pass && a_pass->shaderProperty && a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);
}

StaticInstancing::Verdict StaticInstancing::Classify(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags, BatchKey& o_key)
{
	if (!a_pass || !a_pass->shader || a_pass->shader->shaderType.get() != RE::BSShader::Type::Utility)
		return Verdict::kNotUtility;

	// BSUtilityShader::SetupTechnique derives the raw descriptor as technique - 0x2B (verified in the
	// 1.6.1170 decompile), so the UtilityShaderFlags bits live on that value, not on the technique id.
	const uint32_t descriptor = a_technique - kUtilityTechniqueBase;
	lastDescriptor = descriptor;
	if (descriptor & static_cast<uint32_t>(UF::RenderShadowmap))
		frame.descShadowmap++;
	if (descriptor & static_cast<uint32_t>(UF::RenderDepth))
		frame.descDepth++;
	if (descriptor & (static_cast<uint32_t>(UF::RenderShadowmask) | static_cast<uint32_t>(UF::RenderShadowmaskSpot) | static_cast<uint32_t>(UF::RenderShadowmaskPb) | static_cast<uint32_t>(UF::RenderShadowmaskDpb)))
		frame.descShadowmask++;
	if (descriptor & static_cast<uint32_t>(UF::TreeAnim))
		frame.descTreeAnim++;
	if (descriptor & static_cast<uint32_t>(UF::Skinned))
		frame.descSkinned++;
	if (descriptor & static_cast<uint32_t>(UF::LodLandscape))
		frame.descLodLandscape++;
	if (descriptor & static_cast<uint32_t>(UF::AlphaTest))
		frame.descAlphaTest++;

	const bool isShadowmap = (descriptor & static_cast<uint32_t>(UF::RenderShadowmap)) != 0;
	const bool isDepth = (descriptor & static_cast<uint32_t>(UF::RenderDepth)) != 0;
	// Shadow maps, plus the depth pre-pass when it was enabled at load (biased pre-pass depth and a
	// less-or-equal main pass test replace the bit-exact requirement).
	if (!(isShadowmap || (isDepth && prepassActive && biasArmed)) || (descriptor & kExcludedBits) != 0)
		return Verdict::kTechnique;
	// Shadow-caster culling: drop static casters too small to matter for the shadow map. Skinned
	// geometry (actors) is never culled. The pass is simply not submitted, so its full cost is saved.
	if (settings.MinCasterRadius > 0.0f || settings.MinCasterRadiusFocus > 0.0f) {
		if (auto* geo = a_pass->geometry; geo && !geo->GetGeometryRuntimeData().skinInstance) {
			const float threshold = settings.MinCasterRadius;
			if (threshold > 0.0f && geo->worldBound.radius < threshold)
				return Verdict::kCulled;
		}
	}

	// Tree sway is computed per geometry (BSLeafAnimNode timers); the instance record carries it.
	// Shadow maps only: the main pass computes its own sway and depth-tests for equality.
	if ((descriptor & static_cast<uint32_t>(UF::TreeAnim)) != 0 && (!settings.IncludeTrees || !isShadowmap))
		return Verdict::kTechnique;

	auto* geometry = a_pass->geometry;
	if (!geometry)
		return Verdict::kGeometry;
	// kTriShape draws its whole index range. kMeshLODTriShape draws the sub-range the engine picks
	// from the pass LOD mode it writes into the geometry (batch draw dispatch, case 5); the LOD mode
	// is part of the key and the representative's draw supplies start and count, so it batches too.
	const auto geometryType = static_cast<uint32_t>(geometry->GetType().get());
	if (geometryType != static_cast<uint32_t>(RE::BSGeometry::Type::kTriShape) &&
		(geometryType != static_cast<uint32_t>(RE::BSGeometry::Type::kMeshLODTriShape) || !settings.IncludeMeshLOD)) {
		frame.excludedGeometryByType[geometryType & 15]++;
		return Verdict::kGeometry;
	}

	const auto& runtime = geometry->GetGeometryRuntimeData();
	if (!runtime.rendererData || !runtime.rendererData->vertexBuffer || !runtime.rendererData->indexBuffer)
		return Verdict::kGeometry;

	if (runtime.skinInstance)
		return Verdict::kSkinned;

	// NiAVObject::flags02 & 8 routes the pass through the engine's custom-render path (verified in
	// the 1.6.1170 decompile of RenderPassImmediately). Not the flat layout in VR, so skip the test there.
	if (!REL::Module::IsVR() && (reinterpret_cast<const std::uint8_t*>(geometry)[0x109] & 0x8) != 0)
		return Verdict::kGeometry;

	auto* property = a_pass->shaderProperty;
	if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
		return Verdict::kProperty;
	if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape))
		return Verdict::kProperty;

	// Objects inside their fade range get a per-object stencil reference in SetupGeometry; the
	// dithered fade would then use the representative's value for every instance.
	if (a_pass->accumulationHint == 10)
		return Verdict::kFading;

	o_key = {};
	o_key.rendererData = runtime.rendererData;
	o_key.technique = a_technique;
	o_key.renderFlags = a_renderFlags;
	o_key.propertyFlags = static_cast<uint64_t>(property->flags.get());
	o_key.passState = static_cast<uint32_t>(a_pass->accumulationHint) | (static_cast<uint32_t>(*reinterpret_cast<const std::uint8_t*>(&a_pass->LODMode)) << 8) | (static_cast<uint32_t>(a_pass->extraParam) << 16);

	const bool alphaTested = a_alphaTest || (descriptor & static_cast<uint32_t>(UF::AlphaTest)) != 0;
	if (alphaTested) {
		// The pixel shader samples the material's diffuse texture and the vertex shader applies
		// its texcoord offset, so only exact material clones may share a batch.
		o_key.material = property->GetBaseMaterial();
		uint32_t alpha = 1u << 24;
		if (auto* alphaProperty = runtime.alphaProperty.get())
			alpha |= static_cast<uint32_t>(alphaProperty->alphaThreshold) | (static_cast<uint32_t>(alphaProperty->alphaFlags) << 8);
		o_key.alpha = alpha;
		if (!settings.IncludeAlphaTested)
			return Verdict::kAlphaExcluded;
	}

	return Verdict::kEligible;
}

void StaticInstancing::OnRenderPass(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	using Fn = Hooks::BSBatchRenderer_RenderPassImmediately;

	if (!Active() || flushing || batchDepth == 0 || !a_pass) {
		Fn::func(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	BeginFrameIfNeeded();

	if (settings.AuditCounters && a_pass->shader) {
		const auto shaderType = a_pass->shader->shaderType.get();
		if (shaderType == RE::BSShader::Type::Lighting)
			AuditLightingPass(a_pass, a_technique, a_renderFlags);
		else if (shaderType == RE::BSShader::Type::Water)
			frame.waterPasses++;
		else if (shaderType == RE::BSShader::Type::Effect)
			frame.effectPasses++;
	}

	// Main-pass instancing: pre-pass draws of objects instanced last frame are coalesced into one
	// instanced draw through a dedicated pre-pass permutation (slightly biased depth), and every
	// main-pass draw of such an object runs test-and-write instead of the equality test. Other
	// consumers of the pre-pass depth (terrain blending, screen-space shadows) keep seeing them.
	BatchKey key;
	Verdict verdict;
	if (settings.IncludeLighting && candidateCount > 0 && IsPrepassInstanceCandidate(a_pass, a_technique, a_alphaTest, a_renderFlags, key)) {
		verdict = Verdict::kPrepassEligible;
	} else {
		verdict = Classify(a_pass, a_technique, a_alphaTest, a_renderFlags, key);
		if (verdict == Verdict::kNotUtility && settings.IncludeLighting && a_pass->shader && a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting)
			verdict = ClassifyLighting(a_pass, a_technique, a_alphaTest, a_renderFlags, key);
	}

	if (verdict != Verdict::kNotUtility && verdict != Verdict::kLightingEligible && verdict != Verdict::kLightingExcluded)
		frame.utilityPasses++;

	bool deferThis = false;
	switch (verdict) {
	case Verdict::kNotUtility:
	case Verdict::kLightingExcluded:
		break;
	case Verdict::kPrepassEligible:
		frame.prepassSkipped++;
		deferThis = settings.Enabled && !instanceSRVs.empty();
		break;
	case Verdict::kLightingEligible:
		frame.lightingEligible++;
		// Only objects that were skipped in this frame's pre-pass can be instanced (their depth is
		// not in the buffer, so test-and-write is correct for them); everything else stays vanilla.
		deferThis = settings.Enabled && !instanceSRVs.empty() && IsSkippedThisFrame(a_pass->geometry);
		if (settings.Enabled && !deferThis) {
			// First sighting: count duplicates now so next frame's pre-pass can instance them.
			auto& seen = lightingMeasure[key];
			if (seen.stamp != frameStamp) {
				seen.stamp = frameStamp;
				seen.count = 0;
				lightingMeasureTouched++;
			}
			seen.count++;
			if (seen.count >= settings.MinBatchSize)
				lightingCandidateKeys[key] = frameStamp;
			lightingKeyByGeometry.emplace_back(a_pass->geometry, key);
		}
		break;
	case Verdict::kTechnique:
		frame.excludedTechnique++;
		break;
	case Verdict::kGeometry:
		frame.excludedGeometry++;
		break;
	case Verdict::kSkinned:
		frame.excludedSkinned++;
		break;
	case Verdict::kProperty:
		frame.excludedProperty++;
		break;
	case Verdict::kFading:
		frame.excludedFading++;
		break;
	case Verdict::kCulled:
		frame.culledSmallCasters++;
		return;  // not submitted at all
	case Verdict::kAlphaExcluded:
	case Verdict::kEligible:
		{
			frame.eligible++;
			if (key.alpha != 0)
				frame.eligibleAlpha++;
			if (verdict == Verdict::kAlphaExcluded)
				frame.excludedAlpha++;
			else
				deferThis = settings.Enabled && !instanceSRVs.empty();
			if (!deferThis) {
				// Not batched, so count duplicates here; batched passes are counted at flush.
				auto& seen = measure[key];
				if (seen.stamp != frameStamp) {
					seen.stamp = frameStamp;
					seen.count = 0;
					measureTouched++;
				}
				if (seen.count > 0)
					frame.collapsible++;
				seen.count++;
			}
		}
		break;
	}

	if (deferThis) {
		auto& slot = bucketLookup[key];
		if (slot.generation != flushGeneration) {
			slot.generation = flushGeneration;
			slot.index = static_cast<uint32_t>(buckets.size());
			buckets.push_back(Bucket{ key });
		}
		const uint32_t bucketIndex = slot.index;
		const uint32_t passIndex = static_cast<uint32_t>(deferred.size());
		Bucket& bucket = buckets[bucketIndex];
		if (bucket.first == kNoPass)
			bucket.first = passIndex;
		else
			deferred[bucket.last].nextInBucket = passIndex;
		bucket.last = passIndex;
		bucket.count++;
		deferred.push_back(DeferredPass{ a_pass, a_technique, a_renderFlags, a_alphaTest, verdict == Verdict::kLightingEligible, verdict == Verdict::kPrepassEligible, bucketIndex, kNoPass });
		frame.deferred++;
		if (verdict == Verdict::kLightingEligible)
			frame.lightingDeferred++;
	} else {
		CallWithForcedDepthWrite(a_pass, a_technique, a_alphaTest, a_renderFlags);
	}

	// Last pass of this group: replay before the engine's EndPass resets cull mode and
	// alpha-to-coverage for the group. Also flush before a terrain/non-terrain transition so
	// deferred passes never replay under Terrain Blending's other depth state.
	auto* next = a_pass->passGroupNext;
	if (!next || IsTerrainPass(next) != IsTerrainPass(a_pass))
		Flush();
}

bool StaticInstancing::WriteInstances(uint32_t a_totalInstances)
{
	// Writes every instanced bucket's matrices with one Map. Each bucket starts on a block
	// boundary so its SRV block addresses it; the gap between buckets is left unused.
	if (a_totalInstances == 0 || !instanceBuffer)
		return false;

	auto* context = globals::d3d::context;
	D3D11_MAP mapType;
	if (noOverwriteSupported) {
		if (ringUsed + a_totalInstances > ringCapacity)
			return false;
		mapType = ringDiscardPending ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
	} else {
		// Without NO_OVERWRITE on SRV buffers every flush renames the whole buffer.
		if (a_totalInstances > ringCapacity)
			return false;
		mapType = D3D11_MAP_WRITE_DISCARD;
		ringUsed = 0;
	}

	const auto writeStart = std::chrono::steady_clock::now();
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(instanceBuffer, 0, mapType, 0, &mapped)))
		return false;
	frame.maps++;

	// Same construction as BSShaderUtil::GetXMFromNiPosAdjust followed by TransposeStoreMatrix3x4:
	// rows are [R*s | t - posAdjust], so the shader can repeat the vanilla mul sequence exactly.
	const RE::NiPoint3 posAdjust = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
	const RE::NiPoint3 previousPosAdjust = globals::game::shadowState->GetRuntimeData().previousPosAdjust.getEye();
	auto* base = static_cast<uint8_t*>(mapped.pData);
	for (Bucket& bucket : buckets) {
		if (!bucket.instanced)
			continue;
		bucket.block = ringUsed / kBlockSize;
		auto* out = reinterpret_cast<float*>(base + static_cast<size_t>(ringUsed) * kInstanceStride);
		ringUsed += (bucket.count + kBlockSize - 1) / kBlockSize * kBlockSize;
	for (uint32_t passIndex = bucket.first; passIndex != kNoPass; passIndex = deferred[passIndex].nextInBucket) {
		const RE::NiTransform& world = deferred[passIndex].pass->geometry->world;
		const auto& r = world.rotate.entry;
		const float s = world.scale;
		out[0] = r[0][0] * s;
		out[1] = r[0][1] * s;
		out[2] = r[0][2] * s;
		out[3] = world.translate.x - posAdjust.x;
		out[4] = r[1][0] * s;
		out[5] = r[1][1] * s;
		out[6] = r[1][2] * s;
		out[7] = world.translate.y - posAdjust.y;
		out[8] = r[2][0] * s;
		out[9] = r[2][1] * s;
		out[10] = r[2][2] * s;
		out[11] = world.translate.z - posAdjust.z;
		const DeferredPass& dp = deferred[passIndex];
		if (!dp.lighting && ((bucket.key.technique - kUtilityTechniqueBase) & static_cast<uint32_t>(UF::TreeAnim)) != 0)
			ComputeTreeParams(dp.pass, out + 12);
		else
			out[12] = out[13] = out[14] = out[15] = 0.0f;
		if (dp.lighting) {
			// PreviousWorld rows, camera-relative with the previous frame's adjust (renderFlags bit 4:
			// no previous transform, use the current one), then MaterialData.
			const RE::NiTransform& prev = (dp.renderFlags & 0x10) ? world : dp.pass->geometry->previousWorld;
			const auto& pr = prev.rotate.entry;
			const float ps = prev.scale;
			out[16] = pr[0][0] * ps;
			out[17] = pr[0][1] * ps;
			out[18] = pr[0][2] * ps;
			out[19] = prev.translate.x - previousPosAdjust.x;
			out[20] = pr[1][0] * ps;
			out[21] = pr[1][1] * ps;
			out[22] = pr[1][2] * ps;
			out[23] = prev.translate.y - previousPosAdjust.y;
			out[24] = pr[2][0] * ps;
			out[25] = pr[2][1] * ps;
			out[26] = pr[2][2] * ps;
			out[27] = prev.translate.z - previousPosAdjust.z;
			auto* lightingProperty = static_cast<RE::BSLightingShaderProperty*>(dp.pass->shaderProperty);
			out[28] = lightingProperty->envmapLODFade;
			out[29] = lightingProperty->specularLODFade;
			out[30] = lightingProperty->alpha;
			out[31] = 0.0f;
		} else {
			for (int i = 16; i < 32; ++i)
				out[i] = 0.0f;
		}
		out += 32;
	}
	}
	context->Unmap(instanceBuffer, 0);
	ringDiscardPending = false;
	frame.writeUs += std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - writeStart).count();
	return true;
}

void StaticInstancing::Flush()
{
	if (deferred.empty())
		return;

	using Fn = Hooks::BSBatchRenderer_RenderPassImmediately;
	flushing = true;
	const auto flushStart = std::chrono::steady_clock::now();
	frame.flushes++;
	// Every deferred Lighting pass belongs to an object with instanced pre-pass depth, so the whole
	// replay runs test-and-write under one depth-mode change instead of two per draw.
	const bool lightingFlush = deferred.front().lighting;
	if (lightingFlush)
		BeginForcedDepthWrite();

	// Pass 1: decide which buckets are instanced and how much ring space they need.
	uint32_t totalInstances = 0;
	frame.uniqueKeys += static_cast<uint32_t>(buckets.size());
	for (Bucket& bucket : buckets) {
		const uint32_t count = bucket.count;
		frame.collapsible += count - 1;
		bucket.instanced = count >= settings.MinBatchSize || settings.InstanceSingles;
		if (bucket.instanced)
			totalInstances += (count + kBlockSize - 1) / kBlockSize * kBlockSize;
	}
	const bool written = totalInstances > 0 && WriteInstances(totalInstances);
	if (totalInstances > 0 && !written)
		frame.ringOverflow++;

	// Pass 2: replay in original order; an instanced bucket draws once, at its first pass.
	for (size_t i = 0; i < deferred.size(); ++i) {
		const DeferredPass& d = deferred[i];
		Bucket& bucket = buckets[d.bucket];
		const uint32_t count = bucket.count;

		if (!bucket.instanced || !written) {
			CallWithForcedDepthWrite(d.pass, d.technique, d.alphaTest, d.renderFlags);
			frame.singlesReplayed++;
			continue;
		}

		if (bucket.drawn)
			continue;
		bucket.drawn = true;

		pending.active = true;
		pending.lighting = d.lighting;
		pending.prepass = d.prepass;
		pending.fellBack = false;
		pending.count = count;
		pending.block = bucket.block;
		pending.rendererData = bucket.key.rendererData
			? static_cast<const RE::BSGraphics::TriShape*>(bucket.key.rendererData)
			: nullptr;

		if (d.lighting)
			CallWithForcedDepthWrite(d.pass, d.technique, d.alphaTest, d.renderFlags);
		else
			Fn::func(d.pass, d.technique, d.alphaTest, d.renderFlags);

		if (pending.active) {
			// The engine never reached DrawTriShape for the representative (technique setup
			// failed); the other instances are lost for this frame, the same as vanilla would
			// have skipped them.
			pending = {};
			frame.pendingDropped++;
		} else if (pending.fellBack) {
			// Instanced permutation not ready: the representative was drawn alone, draw the rest.
			pending = {};
			if (d.prepass)
				frame.prepassShaderMisses++;
			else
				frame.lightingShaderMisses++;
			for (uint32_t passIndex = bucket.first; passIndex != kNoPass; passIndex = deferred[passIndex].nextInBucket) {
				if (passIndex == i)
					continue;
				const DeferredPass& p = deferred[passIndex];
				CallWithForcedDepthWrite(p.pass, p.technique, p.alphaTest, p.renderFlags);
			}
		} else {
			frame.instancedDraws++;
			frame.collapsed += count - 1;
			if (d.lighting) {
				frame.lightingInstancedDraws++;
				frame.lightingCollapsed += count - 1;
			}
			if (d.prepass) {
				// These objects now have biased pre-pass depth; their main-pass draws must test-and-write.
				// A vanilla replay (miss, too small a bucket) leaves them out, and the main pass draws
				// them vanilla against their exact pre-pass depth.
				frame.prepassInstancedDraws++;
				for (uint32_t passIndex = bucket.first; passIndex != kNoPass; passIndex = deferred[passIndex].nextInBucket)
					MarkSkipped(deferred[passIndex].pass->geometry);
			}
		}
		// Members of an instanced main-pass batch, and eligible objects that batched this frame, are
		// skipped in next frame's pre-pass so they can be instanced again.
		if (d.lighting && !pending.fellBack)
			for (uint32_t passIndex = bucket.first; passIndex != kNoPass; passIndex = deferred[passIndex].nextInBucket)
				MarkInstanced(deferred[passIndex].pass->geometry);
	}

	if (lightingFlush)
		EndForcedDepthWrite();
	deferred.clear();
	buckets.clear();
	flushGeneration++;
	flushing = false;
	frame.flushUs += std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - flushStart).count();
}

void StaticInstancing::DrawInstanced(RE::BSGraphics::TriShape* a_triShape, uint32_t a_startIndex, uint32_t a_count, uint32_t a_instanceCount, uint32_t a_block)
{
	auto* context = globals::d3d::context;
	auto& shadowState = globals::game::shadowState->GetRuntimeData();

	// Resolved on first use, not at DLL load, so the address library is guaranteed to be ready.
	static REL::Relocation<void (*)(uint32_t)> SetDirtyStates{ REL::RelocationID(75580, 77386) };

	// Same state work as Renderer::DrawTriShape (verified against 1.5.97 and 1.6.1170).
	const uint64_t vertexDesc = *reinterpret_cast<const uint64_t*>(&a_triShape->vertexDesc);
	if (shadowState.vertexDesc != vertexDesc) {
		shadowState.vertexDesc = vertexDesc;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
	}
	if (shadowState.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		shadowState.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	}
	SetDirtyStates(0);

	context->IASetIndexBuffer(reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer), DXGI_FORMAT_R16_UINT, 0);
	const UINT stride = static_cast<UINT>((4 * vertexDesc) & 0x3C);
	const UINT offset = 0;
	ID3D11Buffer* vertexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer);
	context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

	context->VSSetConstantBuffers(kBatchCBSlot, 1, &enabledCB);
	context->VSSetShaderResources(kInstanceSRVSlot, 1, &instanceSRVs[a_block]);

	context->DrawIndexedInstanced(a_count * 3, a_instanceCount, a_startIndex, 0, 0);

	// Later vanilla draws read b13 and must see the disabled marker.
	context->VSSetConstantBuffers(kBatchCBSlot, 1, &disabledCB);
}

void StaticInstancing::DrawInstancedLighting(RE::BSGraphics::TriShape* a_triShape, uint32_t a_startIndex, uint32_t a_count, uint32_t a_instanceCount, uint32_t a_block)
{
	auto* state = globals::state;
	auto* shaderCache = globals::shaderCache;
	auto* context = globals::d3d::context;
	if (!state->currentShader || !shaderCache) {
		pending.fellBack = true;
		return;
	}
	auto* instancedVS = shaderCache->GetVertexShader(*state->currentShader, state->modifiedVertexDescriptor | kLightingInstancedBit);
	auto* instancedPS = shaderCache->GetPixelShader(*state->currentShader, state->modifiedPixelDescriptor | kLightingInstancedBit);
	if (!instancedVS || !instancedPS) {
		pending.fellBack = true;
		return;
	}
	context->VSSetShader(reinterpret_cast<ID3D11VertexShader*>(instancedVS->shader), nullptr, 0);
	context->PSSetShader(reinterpret_cast<ID3D11PixelShader*>(instancedPS->shader), nullptr, 0);
	DrawInstanced(a_triShape, a_startIndex, a_count, a_instanceCount, a_block);
	// Restore the engine's shaders: its technique cache assumes they are still bound.
	auto* vanillaVS = globals::game::currentVertexShader ? *globals::game::currentVertexShader : nullptr;
	auto* vanillaPS = globals::game::currentPixelShader ? *globals::game::currentPixelShader : nullptr;
	context->VSSetShader(vanillaVS ? reinterpret_cast<ID3D11VertexShader*>(vanillaVS->shader) : nullptr, nullptr, 0);
	context->PSSetShader(vanillaPS ? reinterpret_cast<ID3D11PixelShader*>(vanillaPS->shader) : nullptr, nullptr, 0);
}

void StaticInstancing::DrawInstancedPrepass(RE::BSGraphics::TriShape* a_triShape, uint32_t a_startIndex, uint32_t a_count, uint32_t a_instanceCount, uint32_t a_block)
{
	auto* state = globals::state;
	auto* shaderCache = globals::shaderCache;
	auto* context = globals::d3d::context;
	if (!state->currentShader || !shaderCache) {
		pending.fellBack = true;
		return;
	}
	// Vertex shader only: the pre-pass pixel shaders (none, or the alpha-test one) read no instance data.
	auto* instancedVS = shaderCache->GetVertexShader(*state->currentShader, state->modifiedVertexDescriptor | kPrepassInstancedBit);
	if (!instancedVS) {
		pending.fellBack = true;
		return;
	}
	context->VSSetShader(reinterpret_cast<ID3D11VertexShader*>(instancedVS->shader), nullptr, 0);
	DrawInstanced(a_triShape, a_startIndex, a_count, a_instanceCount, a_block);
	auto* vanillaVS = globals::game::currentVertexShader ? *globals::game::currentVertexShader : nullptr;
	context->VSSetShader(vanillaVS ? reinterpret_cast<ID3D11VertexShader*>(vanillaVS->shader) : nullptr, nullptr, 0);
}

void StaticInstancing::Hooks::BSBatchRenderer_RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	globals::features::staticInstancing.OnRenderPass(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

bool StaticInstancing::Hooks::BSBatchRenderer_RenderBatches::thunk(void* a_renderer, uint32_t* a_currentPass, uint32_t* a_bucketIndex, void* a_passIndexList, uint32_t a_renderFlags)
{
	auto& self = globals::features::staticInstancing;
	// While the pre-pass bias is armed every pre-pass draw must see b13 = disabledCB (bias, no
	// instancing). Bind it at the start of every batch group: a once-per-frame bind does not
	// survive to the depth pre-pass when no instanced draw rebinds it.
	if (self.biasArmed && self.disabledCB)
		globals::d3d::context->VSSetConstantBuffers(kBatchCBSlot, 1, &self.disabledCB);
	self.batchDepth++;
	const bool result = func(a_renderer, a_currentPass, a_bucketIndex, a_passIndexList, a_renderFlags);
	self.batchDepth--;
	if (self.batchDepth == 0 && !self.deferred.empty()) {
		// Safety net: the loop ended without a passGroupNext == nullptr pass. EndPass has run,
		// so the group's cull mode may be off for these; better than dropping them.
		self.Flush();
	}
	return result;
}

void StaticInstancing::Hooks::Renderer_DrawTriShape::thunk(RE::BSGraphics::Renderer* a_renderer, RE::BSGraphics::TriShape* a_triShape, uint32_t a_startIndex, uint32_t a_count)
{
	auto& self = globals::features::staticInstancing;
	if (!self.pending.active || a_triShape != self.pending.rendererData) {
		func(a_renderer, a_triShape, a_startIndex, a_count);
		return;
	}
	const uint32_t instanceCount = self.pending.count;
	const uint32_t block = self.pending.block;
	const bool lighting = self.pending.lighting;
	const bool prepass = self.pending.prepass;
	self.pending.active = false;
	const auto drawStart = std::chrono::steady_clock::now();
	struct DrawTimer
	{
		StaticInstancing& self;
		std::chrono::steady_clock::time_point start;
		~DrawTimer() { self.frame.drawUs += std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count(); }
	} drawTimer{ self, drawStart };
	if (lighting || prepass) {
		if (lighting)
			self.DrawInstancedLighting(a_triShape, a_startIndex, a_count, instanceCount, block);
		else
			self.DrawInstancedPrepass(a_triShape, a_startIndex, a_count, instanceCount, block);
		if (self.pending.fellBack)
			func(a_renderer, a_triShape, a_startIndex, a_count);
		return;
	}
	self.pending = {};
	self.DrawInstanced(a_triShape, a_startIndex, a_count, instanceCount, block);
}

void StaticInstancing::Hooks::BSDynamicTriShape_UnlockDynamicData::thunk(void* a_this)
{
	// BSSpinLock at +0x168: owning thread id, then lock count (1.6.1170 decompile of
	// UnlockDynamicData: the release is skipped unless the owner matches the caller).
	const auto* lock = reinterpret_cast<const volatile uint32_t*>(static_cast<const uint8_t*>(a_this) + 0x168);
	const uint32_t owner = lock[0];
	const uint32_t count = lock[1];
	const uint32_t tid = GetCurrentThreadId();
	if (count != 0 && owner != tid) {
		static std::atomic<int> logged{ 0 };
		if (logged.fetch_add(1) < 40) {
			void* frames[24]{};
			const auto n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
			std::string stack;
			for (USHORT i = 0; i < n; ++i) {
				HMODULE module = nullptr;
				char path[MAX_PATH]{};
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(frames[i]), &module) && module && GetModuleFileNameA(module, path, MAX_PATH)) {
					const char* base = std::strrchr(path, '\\');
					stack += std::format(" {}+{:#x}", base ? base + 1 : path, reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module));
				} else {
					stack += std::format(" {:#x}", reinterpret_cast<uintptr_t>(frames[i]));
				}
			}
			const char* name = reinterpret_cast<RE::NiAVObject*>(a_this)->name.c_str();
			logger::warn("[Static Instancing] UnlockDynamicData from thread {:#x} but the lock of '{}' is owned by thread {:#x} (count {}); the engine will leave it locked. Stack:{}", tid, name ? name : "?", owner, count, stack);
		}
	}
	func(a_this);
}

void StaticInstancing::Hooks::Install()
{
	stl::detour_thunk<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100854, 107644));
	stl::detour_thunk<BSBatchRenderer_RenderBatches>(REL::RelocationID(100852, 107642));
	stl::detour_thunk<Renderer_DrawTriShape>(REL::RelocationID(75477, 77263));
	if (REL::Module::IsAE()) {
		// 1.6.1170 only; the SE id is not mapped. Diagnostic for the leaked-lock hang.
		stl::detour_thunk<BSDynamicTriShape_UnlockDynamicData>(REL::RelocationID(70952, 70952));
	}
	logger::info("[Static Instancing] hooks installed");
}
