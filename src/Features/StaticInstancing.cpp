#include "StaticInstancing.h"

#include "Buffer.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/UI.h"

#include <RE/N/NiSkinPartition.h>
#include <imgui.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	StaticInstancing::Settings,
	Enabled,
	IncludeAlphaTested,
	IncludeDepthPrepass,
	InstanceSingles,
	MinBatchSize,
	MaxInstancesPerFrame)

namespace
{
	// Must match kStaticInstancingMagic in Utility.hlsl. A vanilla draw that reads a stale b13
	// sees some other value and takes the normal path.
	constexpr uint32_t kMagic = 0x5A17C0DE;
	constexpr uint32_t kInstanceStride = 48;  // three float4 rows
	constexpr uint32_t kBatchCBSlot = 13;  // b13: unused by the game and by every other feature (b8 is Light Limit Fix)
	constexpr uint32_t kInstanceSRVSlot = 9;
	constexpr uint32_t kUtilityTechniqueBase = 0x2B;
	constexpr uint32_t kBlockSize = 64;  // batch bases are aligned to this many instances

	using UF = SIE::ShaderCache::UtilityShaderFlags;


	// Anything that changes what the vertex shader does with World, or that is not a plain
	// shadow-map pass. RenderDepth is excluded because Terrain Blending sequences those passes.
	constexpr uint32_t kExcludedBits =
		static_cast<uint32_t>(UF::Skinned) |
		static_cast<uint32_t>(UF::LodLandscape) |
		static_cast<uint32_t>(UF::TreeAnim) |
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
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.CPUAccessFlags = 0;

		// Base is always 0: the bound SRV already starts at the batch's first instance.
		const uint32_t enabled[4] = { 0, kMagic, 0, 0 };
		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = enabled;
		if (FAILED(device->CreateBuffer(&desc, &init, &enabledCB))) {
			logger::error("[Static Instancing] enabled constant buffer creation failed");
			ReleaseResources();
			return;
		}
		Util::SetResourceName(enabledCB, "StaticInstancing::enabledCB");

		const uint32_t zeros[4] = { 0, 0, 0, 0 };
		init.pSysMem = zeros;
		if (FAILED(device->CreateBuffer(&desc, &init, &disabledCB))) {
			logger::error("[Static Instancing] disabled constant buffer creation failed");
			ReleaseResources();
			return;
		}
		Util::SetResourceName(disabledCB, "StaticInstancing::disabledCB");
	}

	ringUsed = 0;
	ringDiscardPending = true;
	logger::info("[Static Instancing] resources ready: {} instances in {} blocks, NO_OVERWRITE on SRV buffers {}", ringCapacity, instanceSRVs.size(), noOverwriteSupported ? "supported" : "unsupported (discard per flush)");
}

void StaticInstancing::ReleaseResources()
{
	for (auto*& srv : instanceSRVs)
		SafeRelease(srv);
	instanceSRVs.clear();
	SafeRelease(instanceBuffer);
	SafeRelease(enabledCB);
	SafeRelease(disabledCB);
	ringCapacity = 0;
}

void StaticInstancing::Reset()
{
	// Nothing survives a frame anyway; drop whatever a partially rendered frame left behind.
	deferred.clear();
	buckets.clear();
	bucketLookup.clear();
	pending = {};
	batchDepth = 0;
	flushing = false;
}

void StaticInstancing::PostPostLoad()
{
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
		{ "excludedTechnique", c.excludedTechnique },
		{ "excludedGeometry", c.excludedGeometry },
		{ "excludedSkinned", c.excludedSkinned },
		{ "excludedProperty", c.excludedProperty },
		{ "excludedAlpha", c.excludedAlpha },
		{ "excludedFading", c.excludedFading },
		{ "descShadowmap", c.descShadowmap },
		{ "descDepth", c.descDepth },
		{ "descShadowmask", c.descShadowmask },
		{ "descTreeAnim", c.descTreeAnim },
		{ "descSkinned", c.descSkinned },
		{ "descLodLandscape", c.descLodLandscape },
		{ "descAlphaTest", c.descAlphaTest },
		{ "includeDepthPrepass", settings.IncludeDepthPrepass },
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
	ImGui::Checkbox("Include depth pre-pass", &settings.IncludeDepthPrepass);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Also coalesce the main view's depth pre-pass. The instanced path repeats the vanilla matrix arithmetic so depths match the main pass exactly.");
	}
	ImGui::Checkbox("Instance singles (debug)", &settings.InstanceSingles);
	if (auto tooltip = Util::HoverTooltipWrapper()) {
		ImGui::Text("Sends every eligible draw through the instanced vertex path with one instance. If shadows look identical to vanilla in this mode, the matrix path is correct.");
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
		row("Passes with RenderShadowmap", c.descShadowmap);
		row("Passes with RenderDepth", c.descDepth);
		row("Passes with shadow mask", c.descShadowmask);
		row("Passes with TreeAnim", c.descTreeAnim);
		row("Passes with Skinned", c.descSkinned);
		row("Passes with LodLandscape", c.descLodLandscape);
		row("Passes with AlphaTest", c.descAlphaTest);
		ImGui::EndTable();
	}
	if (c.eligible > 0) {
		const float ratio = 100.0f * static_cast<float>(c.collapsible) / static_cast<float>(c.eligible);
		ImGui::Text("Collapse ratio: %.1f%% of eligible draws", ratio);
	}
}

void StaticInstancing::BeginFrameIfNeeded()
{
	if (!frameChecker.IsNewFrame())
		return;

	last = frame;
	last.uniqueKeys = frame.uniqueKeys + static_cast<uint32_t>(measure.size());
	frame = {};
	measure.clear();

	ringUsed = 0;
	ringDiscardPending = true;

	if (!deferred.empty()) {
		// A group never flushed (loop exited early). Drop it rather than replay into the wrong state.
		deferred.clear();
		buckets.clear();
		bucketLookup.clear();
	}
	pending = {};
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
	if (!(isShadowmap || (isDepth && settings.IncludeDepthPrepass)) || (descriptor & kExcludedBits) != 0)
		return Verdict::kTechnique;

	auto* geometry = a_pass->geometry;
	if (!geometry || geometry->GetType().get() != RE::BSGeometry::Type::kTriShape)
		return Verdict::kGeometry;

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

	BatchKey key;
	const Verdict verdict = Classify(a_pass, a_technique, a_alphaTest, a_renderFlags, key);

	if (verdict != Verdict::kNotUtility)
		frame.utilityPasses++;

	bool deferThis = false;
	switch (verdict) {
	case Verdict::kNotUtility:
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
				if (seen > 0)
					frame.collapsible++;
				seen++;
			}
		}
		break;
	}

	if (deferThis) {
		uint32_t bucketIndex;
		if (auto it = bucketLookup.find(key); it != bucketLookup.end()) {
			bucketIndex = it->second;
		} else {
			bucketIndex = static_cast<uint32_t>(buckets.size());
			buckets.push_back(Bucket{ key, {}, false });
			bucketLookup.emplace(key, bucketIndex);
		}
		buckets[bucketIndex].passes.push_back(static_cast<uint32_t>(deferred.size()));
		deferred.push_back(DeferredPass{ a_pass, a_technique, a_renderFlags, a_alphaTest, bucketIndex });
		frame.deferred++;
	} else {
		Fn::func(a_pass, a_technique, a_alphaTest, a_renderFlags);
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

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(instanceBuffer, 0, mapType, 0, &mapped)))
		return false;

	// Same construction as BSShaderUtil::GetXMFromNiPosAdjust followed by TransposeStoreMatrix3x4:
	// rows are [R*s | t - posAdjust], so the shader can repeat the vanilla mul sequence exactly.
	const RE::NiPoint3 posAdjust = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
	auto* base = static_cast<uint8_t*>(mapped.pData);
	for (Bucket& bucket : buckets) {
		if (!bucket.instanced)
			continue;
		bucket.block = ringUsed / kBlockSize;
		auto* out = reinterpret_cast<float*>(base + static_cast<size_t>(ringUsed) * kInstanceStride);
		ringUsed += (static_cast<uint32_t>(bucket.passes.size()) + kBlockSize - 1) / kBlockSize * kBlockSize;
	for (uint32_t passIndex : bucket.passes) {
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
		out += 12;
	}
	}
	context->Unmap(instanceBuffer, 0);
	ringDiscardPending = false;
	return true;
}

void StaticInstancing::Flush()
{
	if (deferred.empty())
		return;

	using Fn = Hooks::BSBatchRenderer_RenderPassImmediately;
	flushing = true;

	// Pass 1: decide which buckets are instanced and how much ring space they need.
	uint32_t totalInstances = 0;
	frame.uniqueKeys += static_cast<uint32_t>(buckets.size());
	for (Bucket& bucket : buckets) {
		const uint32_t count = static_cast<uint32_t>(bucket.passes.size());
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
		const uint32_t count = static_cast<uint32_t>(bucket.passes.size());

		if (!bucket.instanced || !written) {
			Fn::func(d.pass, d.technique, d.alphaTest, d.renderFlags);
			frame.singlesReplayed++;
			continue;
		}

		if (bucket.drawn)
			continue;
		bucket.drawn = true;

		pending.active = true;
		pending.count = count;
		pending.block = bucket.block;
		pending.rendererData = bucket.key.rendererData
			? static_cast<const RE::BSGraphics::TriShape*>(bucket.key.rendererData)
			: nullptr;

		Fn::func(d.pass, d.technique, d.alphaTest, d.renderFlags);

		if (pending.active) {
			// The engine never reached DrawTriShape for the representative (technique setup
			// failed); the other instances are lost for this frame, the same as vanilla would
			// have skipped them.
			pending = {};
			frame.pendingDropped++;
		} else {
			frame.instancedDraws++;
			frame.collapsed += count - 1;
		}
	}

	deferred.clear();
	buckets.clear();
	bucketLookup.clear();
	flushing = false;
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

void StaticInstancing::Hooks::BSBatchRenderer_RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	globals::features::staticInstancing.OnRenderPass(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

bool StaticInstancing::Hooks::BSBatchRenderer_RenderBatches::thunk(void* a_renderer, uint32_t* a_currentPass, uint32_t* a_bucketIndex, void* a_passIndexList, uint32_t a_renderFlags)
{
	auto& self = globals::features::staticInstancing;
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
	self.pending = {};
	self.DrawInstanced(a_triShape, a_startIndex, a_count, instanceCount, block);
}

void StaticInstancing::Hooks::Install()
{
	stl::detour_thunk<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100854, 107644));
	stl::detour_thunk<BSBatchRenderer_RenderBatches>(REL::RelocationID(100852, 107642));
	stl::detour_thunk<Renderer_DrawTriShape>(REL::RelocationID(75477, 77263));
	logger::info("[Static Instancing] hooks installed");
}
