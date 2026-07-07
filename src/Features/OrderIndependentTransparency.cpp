#include "OrderIndependentTransparency.h"
#include "ShaderCache.h"
#include "State.h"
#include "Upscaling.h"
#include "Features/TerrainBlending.h"
#include "Deferred.h"
#pragma optimize("", off)
NLOHMANN_JSON_SERIALIZE_ENUM(OrderIndependentTransparency::Method,
	{ 
		{ OrderIndependentTransparency::Method::OIT_DISABLED, "Disabled" },
		{ OrderIndependentTransparency::Method::OIT_VISUALIZE, "Visualize" },
		{ OrderIndependentTransparency::Method::OIT_AT, "AT" },
		{ OrderIndependentTransparency::Method::OIT_BLENDED, "Blended" },
		{ OrderIndependentTransparency::Method::OIT_RVO, "RVO" }
	}
)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OrderIndependentTransparency::Settings,
	Method,
	BufferSize,
	MaxLayers)

static void NormalizeSettings(OrderIndependentTransparency::Settings& settings)
{
	if (settings.Method == OrderIndependentTransparency::Method::OIT_BLENDED)
	{
		settings.UsePixelShader = true;
	}
}

std::span<const D3D_SHADER_MACRO> OrderIndependentTransparency::GetShaderDefines() const
{
	// OIT = OIT_METHOD_DEFINES[settings.Method]
	// OIT_NODE_COUNT = settings.MaxLayers
	return { &shaderDefines[0], 1UZ + (settings.Method == Method::OIT_RVO) };
}

static constexpr const char OIT_METHOD_DEFINES[][2] = { "0", "1", "1", "3", "2" };
bool OrderIndependentTransparency::UpdateShaderDefines()
{
	D3D_SHADER_MACRO defines[2] = { 0 };
	defines[0].Name = "OIT";
	defines[0].Definition = OIT_METHOD_DEFINES[(int)settings.Method];
	if (settings.Method == Method::OIT_RVO)
	{
		defines[1].Name = "OIT_NODE_COUNT";
		defines[1].Definition = shaderDefineBuffer;

		// convert MaxLayers to string in shaderDefineBuffer
		for (char& c : shaderDefineBuffer) c = 0;
		std::to_chars(shaderDefineBuffer, shaderDefineBuffer + sizeof(shaderDefineBuffer), GetNodeCount());
	}

	auto SV = [](const char* str) { return str ? std::string_view(str) : std::string_view{}; };

	if (SV(shaderDefines[0].Definition) != SV(defines[0].Definition) || SV(shaderDefines[1].Definition) != SV(defines[1].Definition))
	{
		shaderDefines[0] = defines[0];
		shaderDefines[1] = defines[1];

		auto new_defines = GetShaderDefines();
		auto define_str = new_defines | std::views::transform([](const D3D_SHADER_MACRO& def) {
			if (def.Name && def.Definition)
				return std::string(def.Name) + "=" + std::string(def.Definition) + " ";
			else if (def.Name)
				return std::string(def.Name);
			else
				return std::string{};
		}) | std::views::join_with(',') | std::ranges::to<std::string>();
		logger::info("[OIT] Shader defines updated [{}]: {}", new_defines.size(), define_str);
		return true;
	}
	return false;
}

bool OrderIndependentTransparency::HasShaderDefine(RE::BSShader::Type shaderType)
{
	return shaderType != shaderType;
	// return shaderType == RE::BSShader::Type::Lighting
		//|| shaderType == RE::BSShader::Type::Water
		//|| shaderType == RE::BSShader::Type::Grass
		;
}

struct Main_RenderWorld_RenderTransparency
{
	// static void* __fastcall thunk(RE::NiCamera*, RE::BSShaderAccumulator*, std::int64_t);
	static void thunk(RE::BSShaderAccumulator* accumulator, uint32_t renderFlags);
	static inline REL::Relocation<decltype(thunk)> func;
};

struct Renderer_Flush_OMSetRenderTargets
{
	static void thunk(ID3D11DeviceContext* a_self, UINT a_numViews, ID3D11RenderTargetView** a_ppRenderTargetViews, ID3D11DepthStencilView* a_depthStencilView)
	{
		auto& oit = globals::features::orderIndependentTransparency;
		if (!oit.inAlphaPass)
			a_self->OMSetRenderTargets(a_numViews, a_ppRenderTargetViews, a_depthStencilView);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct Renderer_Flush
{
	static void thunk(RE::BSGraphics::Renderer* renderer, uint8_t flags)
	{
		// Prevent render target change during the alpha pass
		// Effect shader forced RT change in sub_1414EA1B0 + 0x43
		// And called renderer flush in sub_1414EA1B0 + 0x5E
		// So we have to hook in this level
		auto& oit = globals::features::orderIndependentTransparency;
		oit.PreSetStateDirty();
		//if (oit.inAlphaPass)
		//{
		//	globals::game::stateUpdateFlags->set(false, RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		//	globals::game::stateUpdateFlags->set(false, RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);

		//	// Force depth to test only (not write)
		//	auto shadowState = globals::game::shadowState;
		//	GET_INSTANCE_MEMBER(depthStencil, shadowState);
		//	GET_INSTANCE_MEMBER(depthStencilDepthMode, shadowState);
		//	depthStencil = 0;
		//	depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTest;
		//}
		func(renderer, flags);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void OrderIndependentTransparency::PostPostLoad()
{
	logger::info("[OIT] Hooking OrderIndependentTransparency::Main_RenderWorld_RenderTransparency");

	// std::uintptr_t address = REL::RelocationID(100424, 107142).address() + REL::Relocate(0x3E4, 0x3FE);
	// REL::RelocationID(99938, 106583)
	// stl::write_thunk_call<Main_RenderWorld_RenderTransparency>(address);
	stl::detour_thunk<Main_RenderWorld_RenderTransparency>(REL::RelocationID(99940, 106585));

	logger::info("[OIT] Hooking Renderer_Flush");
	// Need RE for SSE/VR
	if (REL::Module::IsAE())
	{
		//REL::Relocation _target{ REL::ID(77247), 0x1A8 };
		//_target.write_call<6>(OMSetRenderTargets_Hook);
		//stl::write_thunk_call<Renderer_Flush_OMSetRenderTargets, 6>(REL::ID(77247).address() + 0x1A8);
		stl::detour_thunk<Renderer_Flush>(REL::RelocationID(77247, 77247));
		REL::IDDatabase::Offset2ID offset2id;
		auto id = offset2id(0x14EA1B0);
		logger::info("[OIT] Effect shader something id {}", id);
	}
}

void OrderIndependentTransparency::DataLoaded()
{
	NormalizeSettings(settings);
	UpdateShaderDefines();
}

void OrderIndependentTransparency::DrawSettings()
{
	struct DirtyFlags
	{
		bool PixelBuffer = false;
		bool ConstantBuffer = false;
		bool CompositionShader = false;
		bool ShaderDefines = false;

		~DirtyFlags()
		{
			auto& oit = globals::features::orderIndependentTransparency;
			if (ConstantBuffer) {
				oit.featureCB.AlphaThreshold = oit.settings.AlphaThreshold;
				oit.featureCB.DepthThreshold = oit.settings.DepthThreshold;
			}
			if (ShaderDefines)
			{
				if (oit.UpdateShaderDefines()) {
					logger::info("[OIT] Shader defines changed, clearing shader cache.");
					globals::shaderCache->Clear();
					if (oit.settings.Method == Method::OIT_RVO && !oit.colorBuffer.has_value())
					{
						PixelBuffer = true;
					}
				}
			}
			if (PixelBuffer || ShaderDefines) {
				auto* renderer = globals::game::renderer;
				auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
				D3D11_TEXTURE2D_DESC mainDesc;
				mainTex.texture->GetDesc(&mainDesc);
				oit.SetupPixelBuffers(mainDesc.Width * mainDesc.Height);
			}
			if (CompositionShader) {
				oit.ClearShaderCache();
			}
		}
	} dirtied;

	struct EnableScope
	{
		bool Disabled;
		EnableScope(bool EnableCondition) : Disabled(!EnableCondition)
		{
			if (Disabled) ImGui::BeginDisabled();
		}
		~EnableScope() 
		{
			if (Disabled) ImGui::EndDisabled();
		}
	};

	if (ImGui::TreeNodeEx("Method", ImGuiTreeNodeFlags_DefaultOpen))
	{
		/*dirtied.ShaderDefines |=*/
		ImGui::RadioButton("Disable", (int*)&settings.Method, Method::OIT_DISABLED);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Turn off OIT without unload it.");
		}
		dirtied.ShaderDefines |= ImGui::RadioButton("Visualization (For performance and tuning)", (int*)&settings.Method, Method::OIT_VISUALIZE);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Visualize the number of transparent layers to blend per pixel\n"
						"Useful to tune performance settings"
						"The color coding is blue, cyan, green, yellow, orange, red, purple, white.");
		}
		dirtied.ShaderDefines |= ImGui::RadioButton("Balanced (Enable)", (int*)&settings.Method, Method::OIT_AT);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("\"Adaptive Transparency\" technique.\n"
						"It provide accurate result before Max Layers is reach.\n"
						"After the limit, a minimal error estimation is used.\n"
						"This method is fast, but may have flicker or artifacts beyond max layer.");
		}
		dirtied.ShaderDefines |= ImGui::RadioButton("Fast Approximation (*)", (int*)&settings.Method, Method::OIT_BLENDED);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("\"Weighted, Blended OIT\" technique.\n"
						"Fast but INACCURATE approximation of the OIT effect.\n"
						"Have MAJOR problem with additive blend layers\n"
						"Like RAIN or SNOW weather where particles getting very close to camera.\n"
						"(*) The performance of this method will be faster in final OIT release.");
		}
		dirtied.ShaderDefines |= ImGui::RadioButton("Quality (*)", (int*)&settings.Method, Method::OIT_RVO);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("\"Multi Layer Alpha Blend\" (Intel AOIT) technique, requires DirectX 11.3+\n"
						"Using 'Rasterizer Order View' feature to avoid flickering, using bounded memory.\n"
						"Default OIT implementation in Unreal Engine.\n"
						"Have MAJOR performance impact compare to other method.");
		}
		ImGui::TreePop();
	}
	NormalizeSettings(settings);
	ImGui::Spacing();
	if (ImGui::TreeNodeEx("Compatibility", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Checkbox("Multiplicative Blend Support (*)", &settings.CaptureMultiplicativeLayer))
		{
			featureCB.Flags = settings.CaptureMultiplicativeLayer ? 1 : 0;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Whether to support multiplicative blend layer as __grayscale__ in OIT.\n"
						"(*) Colored multiplicative layer is __not__ supported.\n");
		}
		ImGui::Checkbox("Enforce Render Target (*)", &settings.OverrideRenderTargets);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Force to override the transparent render target on every draw call.\n"
						"(*) Have a CPU cost, only enable this option if your alpha mesh is disappearing.\n"
						"(*) This option can only affect AE for now, needs more RE for SSE/VR version.");
		}
		{
			EnableScope blendedModeScope(settings.Method != Method::OIT_BLENDED);
			ImGui::Checkbox("Use Pixel Shader (*)", &settings.UsePixelShader);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Use pixel shader instead of compute shader for OIT resolve.\n"
						"(*) Weighted blended OIT always resolves through the pixel shader path.");
			}
		}
		if (!settings.UsePixelShader) ImGui::BeginDisabled();
		dirtied.CompositionShader |= ImGui::Checkbox("Write Depth (*)", &settings.WriteDepth);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Allow OIT composition to write depth when meshes have the 'Write Depth' flag.\n"
						"(*) Requires pixel shader resolve, which blended OIT already uses.");
		}
		if (!settings.UsePixelShader) ImGui::EndDisabled();
		ImGui::TreePop();
	}
	ImGui::Spacing();
	if (ImGui::TreeNodeEx("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Pass Time      = %6.2f ms", passTime);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("CPU Time spend in the engine's transparent pass");
		}
		ImGui::Text("Composite Time = %6.2f ms", compositeTime);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("CPU Time spend issuing the OIT composition draw");
		}
		{
			EnableScope _(settings.Method == Method::OIT_AT);
			dirtied.PixelBuffer |= ImGui::SliderInt("Pixel Buffer Size", (int*)&settings.BufferSize, 2, 16);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Multiplier of the back buffer size for holding transparent layers.\n"
					"You may experience flickering when this buffer is overflowed.\n"
					"Adjust based on your system's performance and memory capacity.");
			}
		}
		{
			EnableScope _(settings.Method == Method::OIT_AT || settings.Method == Method::OIT_RVO);
			int MaxLayers = settings.MaxLayers;
			ImGui::SliderInt("Max Layers", &MaxLayers, 4, 32, "%d", ImGuiSliderFlags_AlwaysClamp);
			MaxLayers = ((MaxLayers + 2) / 4) * 4; // Round to multiple of 4
			if (MaxLayers != (int)settings.MaxLayers) {
				settings.MaxLayers = MaxLayers;
				dirtied.ShaderDefines = true;
				dirtied.CompositionShader = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Maximum layers to blend in OIT composition\n"
					"Layers exceeding this limit may introduce rendering artifacts\n"
					"Increasing this value can have performance impact.");
			}
		}
		dirtied.ConstantBuffer |= ImGui::SliderFloat("Alpha Cutoff", &settings.AlphaThreshold, 0.0f, 0.1f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Alpha cutoff below which pixels are discarded.\n"
						"Increase this value to improve performance slightly but could have noticeable artifact.\n"
						"For example, increasing this value beyond 0.01 may cause rain particles to fade.");
		}
		dirtied.ConstantBuffer |= ImGui::SliderFloat("Depth Cutoff", &settings.DepthThreshold, 0.9f, 1.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Camera space depth threshold to prevent z-fighting on 32 bits float precision limit.\n"
						"Decreasing this value if you notice z-fighting on overlapping transparent surfaces.\n"
						"However, decreasing this value too much may cause some surfaces to disappear.");
		}
		ImGui::SliderFloat("Distance Threshold (BUGGED ATM)", &settings.DistanceThreshold, 0.f, 500'000.f, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Distance to camera before enabling OIT, to exclude large & complex distant volumetric fogs.\n"
						"Try decrease this value if you notice performance drop or flickering at distant foggy landscape."
						"Use `Visualize` mode to help you adjust this value to exclude the distant fogs."
						"1 `unit` = 1.428cm or 0.5625\"");
		}
		ImGui::TreePop();
	}
}

void OrderIndependentTransparency::LoadSettings(json& o_json)
{
	settings = o_json;
	NormalizeSettings(settings);
	featureCB.AlphaThreshold = settings.AlphaThreshold;
	featureCB.DepthThreshold = settings.DepthThreshold;
	featureCB.Flags = settings.CaptureMultiplicativeLayer ? 1 : 0;
	UpdateShaderDefines();
}

void OrderIndependentTransparency::SaveSettings(json& o_json)
{
	o_json = settings;
}

void OrderIndependentTransparency::RestoreDefaultSettings()
{
	settings = {};
	NormalizeSettings(settings);
	featureCB.AlphaThreshold = 0.f;
	featureCB.Flags = 1;
}

void SetupRenderTarget(RE::RENDER_TARGET target, D3D11_TEXTURE2D_DESC texDesc, D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc, D3D11_RENDER_TARGET_VIEW_DESC rtvDesc, D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc, DXGI_FORMAT format, uint bindFlags);

// Create texture with both SRV and RTV
static void CreateTextureSR(std::optional<Texture2D>& texture, std::string_view name, const D3D11_TEXTURE2D_DESC& baseDesc, DXGI_FORMAT format)
{
	auto texDesc = baseDesc;
	texDesc.Format = format;
	texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	texture.emplace(texDesc);
	texture->resource->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)name.size(), name.data());
	texture->CreateSRV(CD3D11_SHADER_RESOURCE_VIEW_DESC(D3D11_SRV_DIMENSION_TEXTURE2D, format));
	texture->CreateRTV(CD3D11_RENDER_TARGET_VIEW_DESC(D3D11_RTV_DIMENSION_TEXTURE2D, format));
}

void OrderIndependentTransparency::SetupResources()
{
	auto* renderer = globals::game::renderer;
	auto* device = globals::d3d::device;
	// When you want to align with the main texture format
	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC mainDesc;
	mainTex.texture->GetDesc(&mainDesc);
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		mainTex.SRV->GetDesc(&srvDesc);
		mainTex.RTV->GetDesc(&rtvDesc);
		mainTex.UAV->GetDesc(&uavDesc);

		SetupRenderTarget(RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA, mainDesc, srvDesc, rtvDesc, uavDesc, mainDesc.Format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	}

	logger::info("[OIT] Setting up Order Independent Transparency resources...");
	{
		auto texDesc = mainDesc;
		texDesc.Format = DXGI_FORMAT_R32_UINT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		try {
			headerBuffer.emplace(texDesc);
		} catch (const DX::com_exception& e) {
			logger::error("Failed to create oit header texture @{}x{}: {}", mainDesc.Width, mainDesc.Height, e.what());
			return;
		}
		headerBuffer->resource->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)sizeof("OIT Header") - 1, "OIT Header");

		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(
			D3D11_SRV_DIMENSION_TEXTURE2D,
			texDesc.Format
		);
		CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(
			D3D11_UAV_DIMENSION_TEXTURE2D,
			texDesc.Format);
		try {
			headerBuffer->CreateSRV(srvDesc);
		} catch (const DX::com_exception& e) {
			logger::error("Failed to create fragment list head SRV: {}", e.what());
			return;
		}
		try {
			headerBuffer->CreateUAV(uavDesc);
		} catch (const DX::com_exception& e) {
			logger::error("Failed to create fragment list head UVA: {}", e.what());
			return;
		}
	}

	{
		UINT numElem = mainDesc.Width * mainDesc.Height;
		SetupPixelBuffers(numElem);
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		depthStencilDesc.StencilEnable = false;
		try {
			DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, depthStencilState.put()));
		} catch (const DX::com_exception& e) {
			logger::error("Failed to create depth stencil state {}", e.what());
			return;
		}
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		try {
			DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, resolveDepthStencilState.put()));
		} catch (const DX::com_exception& e) {
			logger::error("Failed to create depth stencil state {}", e.what());
			return;
		}
		try {
			CreateTextureSR(accumalationBuffer, "OIT Front Accumalation", mainDesc, DXGI_FORMAT_R16G16B16A16_FLOAT);
			CreateTextureSR(accumalationWaterBuffer, "OIT Accumalation", mainDesc, DXGI_FORMAT_R16G16B16A16_FLOAT);
			CreateTextureSR(revealageBuffer, "OIT Revealage", mainDesc, DXGI_FORMAT_R16G16B16A16_FLOAT);
		} catch (const DX::com_exception& e) {
			logger::error("Failed to create weighted blended OIT render targets: {}", e.what());
			return;
		}

		D3D11_BLEND_DESC wboitBlendDesc{};
		wboitBlendDesc.AlphaToCoverageEnable = false;
		wboitBlendDesc.IndependentBlendEnable = true;
		for (auto slot : { 3, 4 }) {
			auto& rt = wboitBlendDesc.RenderTarget[slot];
			rt.BlendEnable = true;
			rt.SrcBlend = D3D11_BLEND_ONE;
			rt.DestBlend = D3D11_BLEND_ONE;
			rt.BlendOp = D3D11_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D11_BLEND_ONE;
			rt.DestBlendAlpha = D3D11_BLEND_ONE;
			rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		{
			auto& rt = wboitBlendDesc.RenderTarget[5];
			rt.BlendEnable = true;
			rt.SrcBlend = D3D11_BLEND_ZERO;
			rt.DestBlend = D3D11_BLEND_SRC_COLOR;
			rt.BlendOp = D3D11_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D11_BLEND_ONE;
			rt.DestBlendAlpha = D3D11_BLEND_ONE;
			rt.BlendOpAlpha = D3D11_BLEND_OP_MIN;
			rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_ALPHA;
		}
		try {
			DX::ThrowIfFailed(device->CreateBlendState(&wboitBlendDesc, wboitBlendState.put()));
		} catch (const DX::com_exception& e) {
			logger::error("Failed to create weighted blended OIT blend state {}", e.what());
			return;
		}

		D3D11_BLEND_DESC blendDesc{};
		blendDesc.AlphaToCoverageEnable = false;
		blendDesc.IndependentBlendEnable = false;
		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		try {
			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, resolveBlendState.put()));
		} catch (const DX::com_exception& e) {
			logger::error("Failed to create blend state {}", e.what());
			return;
		}
	}

	{
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& postWaterCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_WATER_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		mainDepth.texture->GetDesc(&texDesc);
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, NULL, &postWaterCopy.texture));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		mainDepth.depthSRV->GetDesc(&srvDesc);
		DX::ThrowIfFailed(device->CreateShaderResourceView(postWaterCopy.texture, &srvDesc, &postWaterCopy.depthSRV));

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		mainDepth.views[0]->GetDesc(&dsvDesc);
		DX::ThrowIfFailed(device->CreateDepthStencilView(postWaterCopy.texture, &dsvDesc, &postWaterCopy.views[0]));
	}

	CompileShaders();
	logger::info("[OIT] Order Independent Transparency resources setup complete.");
}

void OrderIndependentTransparency::ClearShaderCache()
{
	csAT.detach();
	csVisualize.detach();
	csBlend.detach();
	psAT.detach();
	psVisualize.detach();
	psBlend.detach();
	psROV.detach();
	CompileShaders();
}

uint OrderIndependentTransparency::GetNodeCount() const
{
	uint nodes = std::clamp<uint>(settings.MaxLayers, 1, 32);
	return nodes % 4 == 0 ? nodes : nodes - nodes % 4 + 4;
}

void OrderIndependentTransparency::CompileShaders()
{
	// OIT_NODE_COUNT = MaxLayers
	uint nodes = GetNodeCount();
	char nodesStr[4] = { 0 };
	std::to_chars(nodesStr, nodesStr + 4, nodes);
	const char* writeDepthDefine = settings.WriteDepth ? "1" : "0";

	logger::info("[OIT] Compiling Order Independent Transparency shaders, OIT_NODE_COUNT={}, OIT_WRITE_DEPTH={}...", nodesStr, writeDepthDefine);

	//if (!csAT)
	//{
	//	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITResolve.cs.hlsl", { { "OIT_NODE_COUNT", nodesStr } }, "cs_5_0"))) {
	//		csAT.attach(rawPtr);
	//	} else {
	//		logger::error("Failed to compile Order Independent Transparency resolve compute shader.");
	//		return;
	//	}
	//}
	//if (!csVisualize) {
	//	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITResolve.cs.hlsl", { { "OIT_DEBUG", "1" } }, "cs_5_0"))) {
	//		csVisualize.attach(rawPtr);
	//	} else {
	//		logger::error("Failed to compile Order Independent Transparency visualize compute shader.");
	//		return;
	//	}
	//}
	//if (!csBlend) {
	//	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITResolve.cs.hlsl", { { "OIT_BLENDED", "1" } }, "cs_5_0"))) {
	//		csBlend.attach(rawPtr);
	//	} else {
	//		logger::error("Failed to compile Order Independent Transparency weighted blend compute shader.");
	//		return;
	//	}
	//}
	if (/*settings.Method == OIT_VISUALIZE && */!psVisualize) {
		if (auto rawPtr = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITResolve.ps.hlsl", { { "OIT_DEBUG", "1" } }, "ps_5_0"))) {
			psVisualize.attach(rawPtr);
		} else {
			logger::error("Failed to compile Order Independent Transparency debug pixel shader.");
		}
	}
	if (/*settings.Method == OIT_AT && */!psAT) {
		if (auto rawPtr = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITResolve.ps.hlsl", { { "OIT_AT", "1" }, { "OIT_NODE_COUNT", nodesStr }, { "OIT_WRITE_DEPTH", writeDepthDefine } }, "ps_5_0"))) {
			psAT.attach(rawPtr);
		} else {
			logger::error("Failed to compile Order Independent Transparency resolve pixel shader.");
		}
	}
	if (/*settings.Method == OIT_BLENDED && */!psBlend) {
		if (auto rawPtr = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITResolve.ps.hlsl", { { "OIT_BLENDED", "1" }, { "OIT_WRITE_DEPTH", writeDepthDefine } }, "ps_5_0"))) {
			psBlend.attach(rawPtr);
		} else {
			logger::error("Failed to compile Order Independent Transparency blend pixel shader.");
		}
	}
	if (/*settings.Method == OIT_RVO && */!psROV) {
		if (auto rawPtr = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITResolve.ps.hlsl", { { "OIT_ROV", "1" }, { "OIT_NODE_COUNT", nodesStr }, { "OIT_WRITE_DEPTH", writeDepthDefine } }, "ps_5_0"))) {
			psROV.attach(rawPtr);
		} else {
			logger::error("Failed to compile Order Independent Transparency ROV resolve pixel shader.");
		}
	}
}

static void CreateStructBuffer(std::optional<Buffer>& buffer, std::string_view name, uint elements, uint size, bool counter = false)
{
	CD3D11_BUFFER_DESC bufferDesc(
		elements * size,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		D3D11_USAGE_DEFAULT,
		0,
		D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
		size);
	try {
		buffer.emplace(bufferDesc);
	} catch (const DX::com_exception& e) {
		logger::error("Failed to create {} buffer: {}", name, e.what());
		return;
	}
	buffer->resource->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)name.size(), name.data());

	CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(
		D3D11_SRV_DIMENSION_BUFFER,
		DXGI_FORMAT_UNKNOWN,
		0, elements);
	// D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	// srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	// srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
	// srvDesc.BufferEx.FirstElement = 0;
	// srvDesc.BufferEx.NumElements = elements;
	// srvDesc.BufferEx.Flags = 0;
	try {
		buffer->CreateSRV(srvDesc);
	} catch (const DX::com_exception& e) {
		logger::error("Failed to create {} SRV: {}", name, e.what());
		return;
	}
	if (!buffer->srv) {
		logger::error("Failed to create {} SRV", name);
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = elements;
	uavDesc.Buffer.Flags = counter ? D3D11_BUFFER_UAV_FLAG_COUNTER : 0;
	try {
		buffer->CreateUAV(uavDesc);
	} catch (const DX::com_exception& e) {
		logger::error("Failed to create {} UAV: {}", name, e.what());
		return;
	}
	if (!buffer->uav)
	{
		logger::error("Failed to create {} UAV", name);
	}
}

void OrderIndependentTransparency::SetupPixelBuffers(uint numElem)
{
	logger::info("[OIT] Requested Setup Pixel Buffers {}", numElem);
	if (settings.Method == Method::OIT_RVO)
	{
		uint NodeCount = GetNodeCount();
		uint colorBufferStride = sizeof(uint32_t) * 4 * NodeCount;
		uint depthBufferStride = sizeof(float) * 4 * NodeCount;
		uint colorBufferBytes = colorBufferStride * numElem;
		if (!colorBuffer.has_value() || colorBuffer->desc.ByteWidth != colorBufferBytes || colorBuffer->desc.StructureByteStride != colorBufferStride)
		{
			logger::info("[OIT] Creating AOIT Buffers {} * {}", numElem, NodeCount);
			CreateStructBuffer(colorBuffer, "OIT Color", numElem, colorBufferStride);
			CreateStructBuffer(depthBuffer, "OIT Depth", numElem, depthBufferStride);
		}
	}
	else if (settings.Method == Method::OIT_AT || settings.Method == Method::OIT_VISUALIZE)
	{
		uint BufferSize = settings.BufferSize * numElem;
		if (!nodesBuffer.has_value() || nodesBuffer->desc.ByteWidth != BufferSize * sizeof(FragmentListNode))
		{
			logger::info("[OIT] Creating Fragment List Buffers {} * {}", numElem, settings.BufferSize);
			featureCB.MaxListNodes = BufferSize;
			CreateStructBuffer(nodesBuffer, "OIT Nodes", BufferSize, sizeof(FragmentListNode), true);
		}
	}
}

enum AlphaBlendMode : uint32_t
{
	kAlpha = 1,              //  src.rgb * src.a + dst.rgb * (1 - src.a)
	kAdditive = 2,           //  src.rgb * src.a + dst.rgb *  1
	kMultiplicativeAlpha = 3,//                    dst.rgb * (src.rgb + 1 - src.a)
	kMultiplicative = 4,     //                    dst.rgb *  src.rgb
};

void OrderIndependentTransparency::PreSetStateDirty()
{
	if (!inAlphaPass /*|| !closeEnough*/) {
		return;
	}
	globals::game::stateUpdateFlags->set(false, RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->set(false, RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	if (settings.Method == Method::OIT_BLENDED)
		globals::game::stateUpdateFlags->set(false, RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	// Force depth to test only (not write)
	auto shadowState = globals::game::shadowState;
	GET_INSTANCE_MEMBER(depthStencil, shadowState);
	GET_INSTANCE_MEMBER(depthStencilDepthMode, shadowState);

	// Setup depth write descriptor, depth write will be skipped in capture pass
	//using enum RE::BSGraphics::DepthStencilDepthMode;
	//if (/*settings.WriteDepth && */ (depthStencilDepthMode == kWrite || depthStencilDepthMode == kTestWrite)) {
	//	// logger::info("PreSetStateDirty requests depth write @ draw {}.", drawIndex);
	//	drawWriteDepth = drawIndex;
	//}

	depthStencil = 0;
	depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTest;
}

void OrderIndependentTransparency::PreDrawHack()
{
	using enum State::ExtraFeatureDescriptors;
	static constexpr uint OITAddtiveDescriptor = std::to_underlying(OITAdditive);
	static constexpr uint OITMultiplicativeDescriptor = std::to_underlying(OITMultiplicative);
	static constexpr uint OITMultiplicativeAlphaDescriptor = std::to_underlying(OITMultiplicative) | std::to_underlying(OITAdditive);
	static constexpr uint OITDepthWriteDescriptor = std::to_underlying(OITDepthWrite);
	static constexpr uint OITDisabledDescriptor = std::to_underlying(OITDisabled);

	auto& descriptor = globals::state->permutationData.ExtraFeatureDescriptor;
	descriptor &= ~(OITAddtiveDescriptor | OITMultiplicativeDescriptor | OITDepthWriteDescriptor | OITDisabledDescriptor);

	if (!inAlphaPass/* || !closeEnough*/) {
		return;
	}

	// Distance gating is evaluated in SetupGeometry(), which runs after this hook.
	// Disabling OIT here makes translucent objects bypass capture and show up as overlays in water refraction.

	auto shadowState = globals::game::shadowState;
	GET_INSTANCE_MEMBER(alphaBlendMode, shadowState);
	GET_INSTANCE_MEMBER(depthStencilDepthMode, shadowState);

	if (alphaBlendMode > 4) [[unlikely]] {
		winrt::com_ptr<ID3D11BlendState> blend = nullptr;
		globals::d3d::context->OMGetBlendState(blend.put(), nullptr, nullptr);
		D3D11_BLEND_DESC desc;
		blend->GetDesc(&desc);

		auto dest = desc.RenderTarget[0].DestBlend;
		auto src = desc.RenderTarget[0].SrcBlend;
		auto op = desc.RenderTarget[0].BlendOp;
		
		logger::warn("unknown alpha blend mode {}, DestBlend {} , SrcBlend {}, Op {}", alphaBlendMode, magic_enum::enum_name(dest), magic_enum::enum_name(src), magic_enum::enum_name(op));
	}

	// Blend mode descriptors
	if (alphaBlendMode == AlphaBlendMode::kAdditive)
		descriptor |= OITAddtiveDescriptor;
	else if (alphaBlendMode == AlphaBlendMode::kMultiplicative)
		descriptor |= OITMultiplicativeDescriptor;
	else if (alphaBlendMode == AlphaBlendMode::kMultiplicativeAlpha)
		descriptor |= OITMultiplicativeDescriptor;

	// Setup depth write descriptor, depth write will be skipped in capture pass
	using enum RE::BSGraphics::DepthStencilDepthMode;
	if (depthStencilDepthMode == kWrite || depthStencilDepthMode == kTestWrite || drawWriteDepth)
	{
		descriptor |= OITDepthWriteDescriptor;
		// logger::info("Write depth @ descriptor {}.", descriptor);
	}

	if (settings.Method == Method::OIT_BLENDED)
		globals::d3d::context->OMSetBlendState(wboitBlendState.get(), nullptr, 0xffffffff);

	if (settings.OverrideRenderTargets || !REL::Module::IsAE())
	{
		if (settings.Method == Method::OIT_BLENDED)
			globals::d3d::context->OMSetRenderTargets((UINT)rtvs.size(), rtvs.data(), dsv);
		else
			globals::d3d::context->OMSetRenderTargetsAndUnorderedAccessViews(3, rtvs.data(), dsv, 3, 2 + (settings.Method == OIT_RVO ? 1 : 0), uavs.data(), nullptr);
		lastVS = *globals::game::currentPixelShader;
	}
}

void OrderIndependentTransparency::SetupGeometry(RE::BSShader*, RE::BSRenderPass* pass, uint32_t)
{
	if (!inAlphaPass)
		return;

	if (pass->shaderProperty && pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite)) 
	{
		// TODO:
		// Re-consolidate Z-Buffer Write geometries to a single draw call
		// Skyrim is breaking Z-Buffer Write alpha geometries into multiple sub mesh and draw calls
		// We nolonger need this workaround with proper OIT
		// logger::info("Geometry requests depth write @ {}.", pass->geometry->name);
		drawWriteDepth = true;
	}
	if (closeEnough)
	{
		return;
	}

	const RE::NiBound& geometryBound = pass->geometry->worldBound;
	auto position = geometryBound.center;
	// float distance = position.GetDistance(cameraPos);
	auto viewPos = cameraWorldInverse * position;

	//wchar_t buffer[256];
	//*fmt::format_to_n(buffer, std::size(buffer) - 1, "SetupGeometry Distance<{}>", distance).out = '\0';
	//globals::state->SetPerfMarker(buffer);

	// logger::debug("distance = {}, view position = {}", distance, viewPos);
	//float rdistance = distance - geometryBound.radius;
	if (viewPos.y < settings.DistanceThreshold) {
		closeEnough = true;
		 logger::debug("Close enough for OIT");
	}
}

void OrderIndependentTransparency::RestoreGeometry(RE::BSShader* , RE::BSRenderPass* , uint32_t )
{
	drawWriteDepth = false;
}

void OrderIndependentTransparency::BeginAlphaGroup()
{
	EndWater();

	cameraPos = Util::GetAverageEyePosition();
	auto cameraWorld = RE::PlayerCamera::GetSingleton()->cameraRoot->world;
	cameraWorldInverse = cameraWorld.Invert();
	inAlphaPass = true;
	calls = 0;
	lastVS = nullptr;
	// Keep capture enabled for the whole alpha pass until distance-threshold evaluation
	// is moved earlier than PreDrawHack()/shader permutation setup.
	closeEnough = true;

	logger::debug("Beginning OIT alpha group camera pos = {}, camera world position = {}", cameraPos, cameraWorld.translate);

	static constexpr bool resetUAVCounter = true;

	auto* renderer = globals::game::renderer;
	ID3D11DeviceContext* context = globals::d3d::context;
	ID3D11UnorderedAccessView* headerUAV = headerBuffer->uav.get();
	// context->OMGetRenderTargets(0, NULL, sceneDSV.put());

	// Initialize the first node offset RW UAV with a NULL offset (end of the list)
	static constexpr UINT clearValuesHead[4] = {
		0x0UL,
		0x0UL,
		0x0UL,
		0x0UL
	};

	if (settings.WriteDepth && settings.UsePixelShader)
	{
		// We need main depth (depth after water) in composition
		// Copying it so that we can read from MainCopy and write write to Main (depth cannot be UAV)
		auto& main = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& mainCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_WATER_COPY];
		context->CopyResource(mainCopy.texture, main.texture);
	}

	context->ClearUnorderedAccessViewUint(headerUAV, clearValuesHead);

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& TAAMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
	auto& alphaOnly = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA];
	// Need to capture pre-water depth
	auto& preWaterDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	dsv = preWaterDepth.readOnlyViews[0];
	static constexpr UINT uavcounters[2] = { 1, 1 };

	if (settings.Method == OIT_BLENDED)
	{
		static constexpr float clearAccum[4] = { 0, 0, 0, 0 };
		static constexpr float clearRevealage[4] = { 1, 1, 0, 1 };
		rtvs = { main.RTV, TAAMask.RTV, alphaOnly.RTV, accumalationBuffer->rtv.get(), accumalationWaterBuffer->rtv.get(), revealageBuffer->rtv.get() };
		context->ClearRenderTargetView(rtvs[3], clearAccum);
		context->ClearRenderTargetView(rtvs[4], clearAccum);
		context->ClearRenderTargetView(rtvs[5], clearRevealage);
		context->OMSetRenderTargets((UINT)rtvs.size(), rtvs.data(), dsv);

		using globals::features::terrainBlending;
		ID3D11ShaderResourceView* waterDepthSrv = terrainBlending.loaded ? terrainBlending.depthSRVBackup : renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
		context->PSSetShaderResources(120, 1, &waterDepthSrv);
		context->OMSetBlendState(wboitBlendState.get(), nullptr, 0xffffffff);
	}
	else if (settings.Method == OIT_RVO)
	{
		rtvs = { main.RTV, TAAMask.RTV, alphaOnly.RTV };
		uavs = { headerBuffer->uav.get(), colorBuffer->uav.get(), depthBuffer->uav.get() };
		context->OMSetRenderTargetsAndUnorderedAccessViews(3, rtvs.data(), dsv, 3, 3, uavs.data(), nullptr);
	}
	else
	{
		if (headerBuffer->uav == nullptr || nodesBuffer->uav == nullptr) 
		{
			logger::error("Failed to get UAVs for OIT capture pass.");
		}

		rtvs = { main.RTV, TAAMask.RTV, alphaOnly.RTV };
		uavs = { headerBuffer->uav.get(), nodesBuffer->uav.get(), nullptr };
		context->OMSetRenderTargetsAndUnorderedAccessViews(3, rtvs.data(), dsv, 3, 2, uavs.data(), uavcounters);
	}

	context->OMSetDepthStencilState(depthStencilState.get(), 0xFF);
}

template <typename ShaderType, typename ViewType, size_t N>
struct ScopedShaderResource
{
	template <typename... TArgs>
	ScopedShaderResource(ShaderType* , ViewType* (&_views)[N], TArgs&&... _args)
	{
		if constexpr (std::is_same_v<ShaderType, ID3D11PixelShader>)
		{
			if constexpr (std::is_same_v<ViewType, ID3D11ShaderResourceView>)
			{
				globals::d3d::context->PSSetShaderResources(_args..., N, _views);
			} 
			else if constexpr (std::is_same_v<ViewType, ID3D11RenderTargetView>) 
			{
				globals::d3d::context->OMSetRenderTargets(N, _views, _args...);
			}
		}
		else if constexpr (std::is_same_v<ShaderType, ID3D11ComputeShader>)
		{
			if constexpr (std::is_same_v<ViewType, ID3D11ShaderResourceView>)
			{
				globals::d3d::context->CSSetShaderResources(_args..., N, _views);
			}
			else if constexpr (std::is_same_v<ViewType, ID3D11UnorderedAccessView>)
			{
				globals::d3d::context->CSSetUnorderedAccessViews(_args..., N, _views, nullptr);
			}
		}
	}

	~ScopedShaderResource()
	{
		ViewType* views[N] = { 0 };
		if constexpr (std::is_same_v<ShaderType, ID3D11PixelShader>) {
			if constexpr (std::is_same_v<ViewType, ID3D11ShaderResourceView>) {
				globals::d3d::context->PSSetShaderResources(0, N, views);
			} else if constexpr (std::is_same_v<ViewType, ID3D11RenderTargetView>) {
				globals::d3d::context->OMSetRenderTargets(N, views, nullptr);
			}
		} else if constexpr (std::is_same_v<ShaderType, ID3D11ComputeShader>) {
			if constexpr (std::is_same_v<ViewType, ID3D11ShaderResourceView>) {
				globals::d3d::context->CSSetShaderResources(0, N, views);
			} else if constexpr (std::is_same_v<ViewType, ID3D11UnorderedAccessView>) {
				globals::d3d::context->CSSetUnorderedAccessViews(0, N, views, nullptr);
			}
		}
	}
};

template <typename ShaderType, typename ViewType, size_t N, typename... TArgs>
ScopedShaderResource(ShaderType*, ViewType* (&)[N], TArgs&&...) -> ScopedShaderResource<ShaderType, ViewType, N>;

void OrderIndependentTransparency::EndAlphaGroup()
{
	inAlphaPass = false;
	closeEnough = false;

	logger::debug("End OIT alpha group.");

	TracyD3D11Zone(globals::state->tracyCtx, "OIT Composite");
	ID3D11DeviceContext* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;

	// First unbind resources from UAV in collection phase
	{
		if (settings.Method == OIT_BLENDED) {
			ID3D11RenderTargetView* _rtvs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
			context->OMSetRenderTargets(6, _rtvs, nullptr);
		} else {
			ID3D11RenderTargetView* _rtvs[3] = { nullptr, nullptr, nullptr };
			ID3D11UnorderedAccessView* _uavs[3] = { nullptr, nullptr, nullptr };
			context->OMSetRenderTargetsAndUnorderedAccessViews(3, _rtvs, nullptr, 3, 3, _uavs, nullptr);
		}
	}
	{
		ID3D11ShaderResourceView* _srv = nullptr;
		context->PSSetShaderResources(120, 1, &_srv);
	}

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& alpha = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA];
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& mainDepthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_WATER_COPY];

	using globals::features::terrainBlending;
	ID3D11ShaderResourceView* waterDepthSrv = mainDepth.depthSRV;
	// If we need to write depth in composition pass, we need to use the copied depth
	if (settings.WriteDepth && settings.UsePixelShader) waterDepthSrv =  mainDepthCopy.depthSRV;
	// Water depth is rendered at kMain after water pass
	// But mainDepth.depthSRV was REDIRECTED by terrain blending to its own copy (for UAV access)
	// At this point, we need to access the actual main depth as SRV
	else if (terrainBlending.loaded) waterDepthSrv = terrainBlending.depthSRVBackup;

	const bool usePixelShader = settings.UsePixelShader;
	if (usePixelShader)
	{
		ID3D11PixelShader* shader = nullptr;
		switch (settings.Method) {
		case Method::OIT_AT:
			shader = psAT.get();
			break;
		case Method::OIT_BLENDED:
			shader = psBlend.get();
			break;
		case Method::OIT_VISUALIZE:
			shader = psVisualize.get();
			break;
		case Method::OIT_RVO:
			shader = psROV.get();
			break;
		}

		// Set up viewport for fullscreen rendering
		auto screenSize = globals::state->screenSize;

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		// Set up Input Assembler for fullscreen triangle
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Set up vertex shader
		context->VSSetShader(globals::features::upscaling.GetUpscaleVS(), nullptr, 0);

		// Set up rasterizer and blend states
		context->RSSetState(globals::features::upscaling.upscaleRasterizerState.get());
		context->OMSetBlendState(resolveBlendState.get(), nullptr, 0xffffffff);
		context->OMSetDepthStencilState(resolveDepthStencilState.get(), 1);

		// Set up pixel shader resources
		ID3D11RenderTargetView* _rtvs[2] = { main.RTV, alpha.RTV };
		ID3D11DepthStencilView* _dsv = nullptr;
		if (settings.WriteDepth && usePixelShader) {
			_dsv = mainDepth.views[0];
		}
		ScopedShaderResource rtvGuard(shader, _rtvs, _dsv);

		context->PSSetShader(shader, nullptr, 0);

		if (settings.Method == Method::OIT_BLENDED)
		{
			ID3D11ShaderResourceView* srvs[4] = {
				accumalationBuffer->srv.get(),
				accumalationWaterBuffer->srv.get(),
				revealageBuffer->srv.get(),
				waterDepthSrv
			};
			ScopedShaderResource srvGuard(shader, srvs, 0);
			context->Draw(3, 0);
		}
		else
		{
			ID3D11ShaderResourceView* srvs[4] = { waterDepthSrv, headerBuffer->srv.get(), nullptr, nullptr };
			if (settings.Method == OIT_RVO)
			{
				srvs[2] = colorBuffer->srv.get();
				srvs[3] = depthBuffer->srv.get();
			}
			else
			{
				srvs[2] = nodesBuffer->srv.get();
			}
			ScopedShaderResource srvGuard(shader, srvs, 0);
			context->Draw(3, 0);
		}

		context->PSSetShader(nullptr, nullptr, 0);
		context->VSSetShader(nullptr, nullptr, 0);
	}
	else
	{
		ID3D11ComputeShader* shader = nullptr;
		switch (settings.Method) {
			case Method::OIT_AT:
				shader = csAT.get();
				break;
			case Method::OIT_BLENDED:
				shader = csBlend.get();
				break;
			case Method::OIT_VISUALIZE:
				shader = csVisualize.get();
				break;
		}
		context->CSSetShader(shader, NULL, 0);

		ID3D11ShaderResourceView* srvs[4] = { waterDepthSrv, headerBuffer->srv.get(), nullptr, nullptr };
		if (settings.Method == OIT_RVO) {
			srvs[2] = colorBuffer->srv.get();
			srvs[3] = depthBuffer->srv.get();
		} else {
			srvs[2] = nodesBuffer->srv.get();
		}
		ID3D11UnorderedAccessView* _uavs[2] = { main.UAV, alpha.UAV };
		ScopedShaderResource srvGuard(shader, srvs, 0);
		ScopedShaderResource uavGuard(shader, _uavs, 0);
		auto dispatchCount = Util::GetScreenDispatchCount();
		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

		context->CSSetShader(nullptr, nullptr, 0);
	}	

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}

void OrderIndependentTransparency::BeginWater()
{
	if (settings.Method == Method::OIT_DISABLED)
		return;
	auto* renderer = globals::game::renderer;
	ID3D11DeviceContext* context = globals::d3d::context;
	auto& alpha = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA];
	ID3D11ShaderResourceView* srv[] = { alpha.SRV };
	context->PSSetShaderResources(66, ARRAYSIZE(srv), srv);
	// context->PSSetSamplers(66, 1, &globals::deferred->linearSampler);
}

void OrderIndependentTransparency::EndWater()
{
	ID3D11DeviceContext* context = globals::d3d::context;
	ID3D11ShaderResourceView* srv[] = { nullptr };
	context->PSSetShaderResources(66, ARRAYSIZE(srv), srv);
}

void Main_RenderWorld_RenderTransparency::thunk(RE::BSShaderAccumulator* accumulator, uint32_t flags)
{
	using namespace std::chrono;
	using clock = high_resolution_clock;
	auto& oit = globals::features::orderIndependentTransparency;
	bool began = false;
	auto passBegin = clock::now();
	if (globals::shaderCache->IsEnabled() && globals::state->inWorld && oit.loaded && oit.settings.Method != OrderIndependentTransparency::Method::OIT_DISABLED)
	{
		began = true;
		// Override the pixel shaders & OM state for OIT
		oit.BeginAlphaGroup();
	}

	// Render the alpha blending geometries
	{
		TracyD3D11Zone(globals::state->tracyCtx, "Transparency");
		(*func)(accumulator, flags);
	}
	auto passEnd = clock::now();
	oit.passTime = duration_cast<duration<float, std::milli>>(passEnd - passBegin).count();

	if (began)
	{
		// OIT resolve and blend in image space
		oit.EndAlphaGroup();
		oit.compositeTime = duration_cast<duration<float, std::milli>>(clock::now() - passEnd).count();
	} else {
		oit.compositeTime = 0;
	}
}
