#include "CloudShadows.h"

#include "Effect11.h"
#include "Effect11/SettingManager.h"
#include "../I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"

#include <d3dcompiler.h>

#define I18N_KEY_PREFIX "feature.cloud_shadows."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudShadows::Settings,
	Opacity)

void CloudShadows::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("temporal_smoothing"), "Temporal Smoothing"), &enableTemporalSmoothing);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("temporal_smoothing_tooltip"),
							  "Smooths the cloud occlusion cubemap over time. The cubemap updates one face per frame, so shadows and cloud scattering can visibly flash when the sun sits behind a cloud edge; smoothing spreads those steps over a few frames."));
	}

	if (globals::features::effect11.loaded) {
		auto& enb = globals::features::effect11;
		if (enb.enableEffect) {
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Settings are currently managed by ENB.");
			return;
		}
	}

	ImGui::SliderFloat(T(TKEY("opacity"), "Opacity"), &settings.Opacity, 0.0f, 4.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("opacity_tooltip"),
							  "Higher values make cloud shadows darker."));
	}
}

#undef I18N_KEY_PREFIX

void CloudShadows::LoadSettings(json& o_json)
{
	settings = o_json;
	if (o_json.contains("EnableTemporalSmoothing") && o_json["EnableTemporalSmoothing"].is_boolean())
		enableTemporalSmoothing = o_json["EnableTemporalSmoothing"];
}

void CloudShadows::SaveSettings(json& o_json)
{
	o_json = settings;
	o_json["EnableTemporalSmoothing"] = enableTemporalSmoothing;
}

void CloudShadows::RestoreDefaultSettings()
{
	settings = {};
	enableTemporalSmoothing = true;
}

CloudShadows::Settings CloudShadows::GetCommonBufferData()
{
	if (!loaded)
		return settings;

	auto data = settings;

	if (globals::features::effect11.loaded) {
		auto& enb = globals::features::effect11;
		if (enb.enableEffect) {
			auto& settingManager = SettingManager::GetSingleton();
			if (settingManager.GetValue<bool>("EnableCloudShadows", "EFFECT")) {
				data.Opacity = settingManager.GetInterpolatedTimeOfDayValue("Amount", "CLOUDSHADOWS");
			} else {
				data.Opacity = 0.0f;
			}
		}
	}

	return data;
}

void CloudShadows::CheckResourcesSide(int side)
{
	static Util::FrameChecker frame_checker[6];
	if (!frame_checker[side].IsNewFrame())
		return;

	if (previouslyRenderedSide >= 0 && previouslyRenderedSide != side)
		PropagateToCompletion(previouslyRenderedSide);
	previouslyRenderedSide = side;

	auto context = globals::d3d::context;

	float black[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(cloudShadowLayerRTVs[0][side], black);
	renderedLayersMask[side] = 0;
}

void CloudShadows::PropagateToCompletion(int side)
{
	uint32_t mask = renderedLayersMask[side];
	unsigned long highBit;
	int fromLayer = _BitScanReverse(&highBit, mask) ? static_cast<int>(highBit) : 0;

	auto context = globals::d3d::context;

	uint32_t newLayers = mask & ~globalRenderedMask;
	if (newLayers) {
		unsigned long bit;
		uint32_t remaining = newLayers;
		while (_BitScanForward(&bit, remaining)) {
			int newLayer = static_cast<int>(bit);
			for (int otherSide = 0; otherSide < 6; otherSide++) {
				if (otherSide == side)
					continue;
				if (renderedLayersMask[otherSide] & (1u << newLayer))
					continue;
				uint32_t belowMask = renderedLayersMask[otherSide] & ((1u << newLayer) - 1);
				unsigned long nearestBit;
				int srcLayer = _BitScanReverse(&nearestBit, belowMask) ? static_cast<int>(nearestBit) : 0;
				UINT otherSub = D3D11CalcSubresource(0, otherSide, cubemapMipLevels);
				context->CopySubresourceRegion(
					texCloudShadowLayers[newLayer]->resource.get(), otherSub, 0, 0, 0,
					texCloudShadowLayers[srcLayer]->resource.get(), otherSub, nullptr);
				renderedLayersMask[otherSide] |= (1u << newLayer);
			}
			remaining &= ~(1u << bit);
		}
		globalRenderedMask |= mask;
	}

	if (fromLayer < kMaxCloudLayers - 1) {
		UINT subresource = D3D11CalcSubresource(0, side, cubemapMipLevels);
		context->CopySubresourceRegion(
			texCloudShadowLayers[kMaxCloudLayers - 1]->resource.get(), subresource, 0, 0, 0,
			texCloudShadowLayers[fromLayer]->resource.get(), subresource, nullptr);
	}
}

void CloudShadows::SkyShaderHacks()
{
	if (overrideSky) {
		auto renderer = globals::game::renderer;
		auto context = globals::d3d::context;

		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		// render targets
		ID3D11RenderTargetView* rtvs[4];
		ID3D11DepthStencilView* dsv;
		context->OMGetRenderTargets(3, rtvs, &dsv);

		int side = -1;
		for (int i = 0; i < 6; ++i)
			if (rtvs[0] == reflections.cubeSideRTV[i]) {
				side = i;
				break;
			}
		if (side == -1)
			return;

		CheckResourcesSide(side);

		int layer = currentLayerForDraw;

		unsigned long highBit;
		int prevLayer = _BitScanReverse(&highBit, renderedLayersMask[side]) ? static_cast<int>(highBit) : -1;

		UINT subresource = D3D11CalcSubresource(0, side, cubemapMipLevels);

		int fromLayer = std::max(prevLayer, 0);

		context->CopyResource(texSelfShadowCopy->resource.get(), texCloudShadowLayers[layer]->resource.get());

		if (layer > 0) {
			context->CopySubresourceRegion(
				texCloudShadowLayers[layer]->resource.get(), subresource, 0, 0, 0,
				texCloudShadowLayers[fromLayer]->resource.get(), subresource, nullptr);
		}

		ID3D11ShaderResourceView* selfShadowSrv = texSelfShadowCopy->srv.get();
		context->PSSetShaderResources(26, 1, &selfShadowSrv);

		rtvs[3] = cloudShadowLayerRTVs[layer][side];
		context->OMSetRenderTargets(4, rtvs, nullptr);

		float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		UINT sampleMask = 0xffffffff;

		context->OMSetBlendState(cloudShadowBlendState, blendFactor, sampleMask);

		auto cubemapDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS];
		context->PSSetShaderResources(17, 1, &cubemapDepth.depthSRV);

		// Release COM objects to prevent memory leaks
		for (int i = 0; i < 3; ++i) {
			if (rtvs[i])
				rtvs[i]->Release();
		}
		if (dsv)
			dsv->Release();

		renderedLayersMask[side] |= (1u << layer);

		overrideSky = false;
	}
}

int CloudShadows::FindCloudLayer(RE::BSRenderPass* Pass)
{
	auto sky = globals::game::sky;
	if (!sky || !sky->clouds)
		return -1;

	for (int i = 0; i < kMaxCloudLayers; i++) {
		if (sky->clouds->clouds[i].get() == Pass->geometry)
			return i;
	}
	return -1;
}

void CloudShadows::ModifySky(RE::BSRenderPass* Pass)
{
	auto shadowState = globals::game::shadowState;

	auto& cubeMapRenderTarget = shadowState->GetRuntimeData().cubeMapRenderTarget;

	auto skyProperty = static_cast<const RE::BSSkyShaderProperty*>(Pass->shaderProperty);

	if (skyProperty->uiSkyObjectType != RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS)
		return;

	int layer = FindCloudLayer(Pass);
	if (layer < 0)
		return;

	if (cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) {
		currentLayerForDraw = layer;
		overrideSky = true;
	} else {
		auto context = globals::d3d::context;
		ID3D11ShaderResourceView* srv = texCloudShadowLayers[layer]->srv.get();
		context->PSSetShaderResources(26, 1, &srv);
	}
}

void CloudShadows::ReflectionsPrepass()
{
}

ID3D11ShaderResourceView* CloudShadows::BlendOcclusionHistory()
{
	ID3D11ShaderResourceView* fallback = texCubemapCloudOccCopy->srv.get();

	if (!enableTemporalSmoothing || temporalBlendUnavailable || !texCloudOccAccum || !cloudOccCopyArraySRV) {
		accumPrimed = false;
		return fallback;
	}

	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	if (!temporalBlendCS) {
		static const char shaderSource[] = R"(
Texture2DArray<float> Src : register(t0);
RWTexture2DArray<float> Accum : register(u0);
cbuffer BlendCB : register(b0)
{
	float BlendFactor;
	float Width;
	float Height;
	float pad;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	int2 maxCoord = int2(int(Width) - 1, int(Height) - 1);
	float sum = 0.0;
	float weightSum = 0.0;
	[unroll] for (int y = -1; y <= 1; y++) {
		[unroll] for (int x = -1; x <= 1; x++) {
			int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), maxCoord);
			float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
			sum += Src[uint3(p, id.z)] * w;
			weightSum += w;
		}
	}
	Accum[id] = lerp(Accum[id], sum / weightSum, BlendFactor);
}
)";
		winrt::com_ptr<ID3DBlob> csBlob, errorBlob;
		HRESULT hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, "CloudShadowsTemporalBlendCS", nullptr, nullptr,
			"main", "cs_5_0", 0, 0, csBlob.put(), errorBlob.put());
		if (FAILED(hr)) {
			logger::error("[Cloud Shadows] Failed to compile temporal blend shader: {}",
				errorBlob ? static_cast<char*>(errorBlob->GetBufferPointer()) : "unknown error");
			temporalBlendUnavailable = true;
			return fallback;
		}
		hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &temporalBlendCS);
		if (FAILED(hr)) {
			logger::error("[Cloud Shadows] Failed to create temporal blend shader");
			temporalBlendUnavailable = true;
			return fallback;
		}
		Util::SetResourceName(temporalBlendCS, "CloudShadows::TemporalBlendCS");
	}

	if (!temporalBlendCB) {
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = 16;
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &temporalBlendCB))) {
			logger::error("[Cloud Shadows] Failed to create temporal blend constant buffer");
			temporalBlendUnavailable = true;
			return fallback;
		}
		Util::SetResourceName(temporalBlendCB, "CloudShadows::TemporalBlendCB");
	}

	auto now = std::chrono::steady_clock::now();
	float deltaMs = std::chrono::duration<float, std::milli>(now - lastAccumUpdate).count();
	lastAccumUpdate = now;

	constexpr float halfLifeMs = 100.0f;
	float blendFactor = 1.0f;
	if (accumPrimed && deltaMs < 1000.0f)
		blendFactor = 1.0f - std::exp2f(-deltaMs / halfLifeMs);

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(temporalBlendCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		accumPrimed = false;
		return fallback;
	}
	auto* cbData = static_cast<float*>(mapped.pData);
	cbData[0] = blendFactor;
	cbData[1] = static_cast<float>(texCloudOccAccum->desc.Width);
	cbData[2] = static_cast<float>(texCloudOccAccum->desc.Height);
	cbData[3] = 0.0f;
	context->Unmap(temporalBlendCB, 0);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(25, 1, &nullSRV);
	context->CSSetShaderResources(25, 1, &nullSRV);

	ID3D11UnorderedAccessView* uav = texCloudOccAccum->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &cloudOccCopyArraySRV);
	context->CSSetConstantBuffers(0, 1, &temporalBlendCB);
	context->CSSetShader(temporalBlendCS, nullptr, 0);

	const auto& desc = texCloudOccAccum->desc;
	context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 6);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetShader(nullptr, nullptr, 0);

	accumPrimed = true;
	return texCloudOccAccum->srv.get();
}

void CloudShadows::EarlyPrepass()
{
	if (previouslyRenderedSide >= 0) {
		PropagateToCompletion(previouslyRenderedSide);
		previouslyRenderedSide = -1;
	}

	auto sky = globals::game::sky;
	if (!sky || (sky->mode.get() != RE::Sky::Mode::kFull) || !sky->currentClimate)
		return;

	auto context = globals::d3d::context;

	context->CopyResource(texCubemapCloudOccCopy->resource.get(), texCloudShadowLayers[kMaxCloudLayers - 1]->resource.get());

	ID3D11ShaderResourceView* srv = BlendOcclusionHistory();
	context->PSSetShaderResources(25, 1, &srv);
	context->CSSetShaderResources(25, 1, &srv);
}

void CloudShadows::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};

		reflections.texture->GetDesc(&texDesc);
		reflections.SRV->GetDesc(&srvDesc);

		texDesc.Format = srvDesc.Format = DXGI_FORMAT_R8_UNORM;
		cubemapMipLevels = texDesc.MipLevels;

		for (int layer = 0; layer < kMaxCloudLayers; ++layer) {
			char name[64];
			snprintf(name, sizeof(name), "CloudShadows::Layer[%d]", layer);
			texCloudShadowLayers[layer] = new Texture2D(texDesc, name);
			texCloudShadowLayers[layer]->CreateSRV(srvDesc);

			for (int face = 0; face < 6; ++face) {
				reflections.cubeSideRTV[face]->GetDesc(&rtvDesc);
				rtvDesc.Format = texDesc.Format;
				DX::ThrowIfFailed(device->CreateRenderTargetView(texCloudShadowLayers[layer]->resource.get(), &rtvDesc, &cloudShadowLayerRTVs[layer][face]));
				Util::SetResourceName(cloudShadowLayerRTVs[layer][face], "CloudShadows::Layer[%d] RTV[%d]", layer, face);
			}
		}

		texCubemapCloudOccCopy = new Texture2D(texDesc, "CloudShadows::CubemapCloudOccCopy");
		texCubemapCloudOccCopy->CreateSRV(srvDesc);

		texSelfShadowCopy = new Texture2D(texDesc, "CloudShadows::SelfShadowCopy");
		texSelfShadowCopy->CreateSRV(srvDesc);

		{
			UINT formatSupport = 0;
			if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R32_FLOAT, &formatSupport)) ||
				!(formatSupport & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ||
				!(formatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
				logger::warn("[Cloud Shadows] R32_FLOAT sampling/UAV unsupported, temporal smoothing disabled");
				temporalBlendUnavailable = true;
			} else {
				D3D11_TEXTURE2D_DESC accumDesc = texDesc;
				accumDesc.Format = DXGI_FORMAT_R32_FLOAT;
				accumDesc.MipLevels = 1;
				accumDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				accumDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
				texCloudOccAccum = new Texture2D(accumDesc, "CloudShadows::CloudOccAccum");

				D3D11_SHADER_RESOURCE_VIEW_DESC accumSrvDesc = srvDesc;
				accumSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
				accumSrvDesc.TextureCube.MostDetailedMip = 0;
				accumSrvDesc.TextureCube.MipLevels = 1;
				texCloudOccAccum->CreateSRV(accumSrvDesc);

				D3D11_UNORDERED_ACCESS_VIEW_DESC accumUavDesc{};
				accumUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
				accumUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
				accumUavDesc.Texture2DArray.MipSlice = 0;
				accumUavDesc.Texture2DArray.FirstArraySlice = 0;
				accumUavDesc.Texture2DArray.ArraySize = 6;
				texCloudOccAccum->CreateUAV(accumUavDesc);

				D3D11_SHADER_RESOURCE_VIEW_DESC copyArraySrvDesc{};
				copyArraySrvDesc.Format = texDesc.Format;
				copyArraySrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				copyArraySrvDesc.Texture2DArray.MostDetailedMip = 0;
				copyArraySrvDesc.Texture2DArray.MipLevels = 1;
				copyArraySrvDesc.Texture2DArray.FirstArraySlice = 0;
				copyArraySrvDesc.Texture2DArray.ArraySize = 6;
				DX::ThrowIfFailed(device->CreateShaderResourceView(texCubemapCloudOccCopy->resource.get(), &copyArraySrvDesc, &cloudOccCopyArraySRV));
				Util::SetResourceName(cloudOccCopyArraySRV, "CloudShadows::CubemapCloudOccCopy Array SRV");
			}
		}
	}
	{
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = false;
		blendDesc.IndependentBlendEnable = false;

		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &cloudShadowBlendState));
		Util::SetResourceName(cloudShadowBlendState, "CloudShadows::BlendState");
	}
}

void CloudShadows::Hooks::BSSkyShader_SetupMaterial::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::state->UpdateSkyShaderPermutation(Pass);
	globals::features::cloudShadows.ModifySky(Pass);
	func(This, Pass, RenderFlags);
}
