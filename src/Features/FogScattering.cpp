#include "FogScattering.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.fog_scattering."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FogScattering::Settings,
	Enable,
	Intensity,
	Radius,
	DensityThreshold,
	MaxLuminance,
	UseManualFog,
	ManualFogNear,
	ManualFogFar,
	ManualFogPower,
	ManualFogClamp)

namespace
{
	constexpr uint32_t kMaxMipCount = 7;
	constexpr uint32_t kMinMipExtent = 8;
}

void FogScattering::RestoreDefaultSettings()
{
	settings = {};
}

void FogScattering::LoadSettings(json& o_json)
{
	settings = o_json;
}

void FogScattering::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FogScattering::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable"), "Enable Fog Scattering"), &settings.Enable);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"),
							  "Blends the scene toward a blur of itself in proportion to how much fog sits in front of "
							  "each pixel, approximating the multiple scattering that real fog performs. Costs one "
							  "half-resolution pyramid and one full-resolution composite."));
	}

	ImGui::BeginDisabled(!settings.Enable);

	ImGui::SliderFloat(T(TKEY("intensity"), "Intensity"), &settings.Intensity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("intensity_tooltip"),
							  "How far a fully fogged pixel is blended toward the scattered result. Lower this first if "
							  "distant detail reads as mushy rather than hazy."));
	}

	ImGui::SliderFloat(T(TKEY("radius"), "Scattering Spread"), &settings.Radius, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("radius_tooltip"),
							  "How wide the scattering blur grows at full fog density. Larger values spread light "
							  "further from bright features but also bleed silhouettes more."));
	}

	ImGui::SliderFloat(T(TKEY("density_threshold"), "Density Threshold"), &settings.DensityThreshold, 0.0f, 0.95f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("density_threshold_tooltip"),
							  "Fog opacity below which nothing is scattered. Raise it to keep near and mid-range "
							  "geometry completely sharp on lightly hazy days."));
	}

	ImGui::SliderFloat(T(TKEY("max_luminance"), "Highlight Clamp"), &settings.MaxLuminance, 1.0f, 64.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("max_luminance_tooltip"),
							  "Upper bound on the luminance fed into the blur. Stops a specular highlight or the sun "
							  "from smearing a large bright halo across the fog."));
	}

	ImGui::Separator();

	ImGui::Checkbox(T(TKEY("use_manual_fog"), "Override Fog Curve"), &settings.UseManualFog);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_manual_fog_tooltip"),
							  "By default the density estimate follows the engine's own fog curve. Override it when "
							  "another mod replaces that fog and the automatic estimate no longer matches what you see."));
	}

	if (settings.UseManualFog) {
		ImGui::SliderFloat(T(TKEY("manual_fog_near"), "Fog Near"), &settings.ManualFogNear, 0.0f, 65536.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("manual_fog_far"), "Fog Far"), &settings.ManualFogFar, 1.0f, 163840.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("manual_fog_power"), "Fog Power"), &settings.ManualFogPower, 0.05f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("manual_fog_clamp"), "Fog Maximum"), &settings.ManualFogClamp, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	}

	ImGui::Text("%s", T(TKEY("resolved_curve"), "Resolved fog curve"));
	ImGui::Text("  near %.0f   range %.0f   power %.2f   max %.2f",
		resolvedFogParams.x,
		resolvedFogParams.y > 0.0f ? 1.0f / resolvedFogParams.y : 0.0f,
		resolvedFogParams.z,
		resolvedFogParams.w);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("resolved_curve_tooltip"),
							  "The curve actually driving the density estimate this frame. A maximum of zero means no "
							  "fog is active and the pass is skipped entirely."));
	}

	ImGui::EndDisabled();
}

void FogScattering::SetupResources()
{
	D3D11_BUFFER_DESC bufferDesc{};
	bufferDesc.ByteWidth = sizeof(FogScatteringCB);
	bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	fogScatteringCB = std::make_unique<ConstantBuffer>(bufferDesc, "FogScattering::ConstantBuffer");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));
	Util::SetResourceName(linearClampSampler.get(), "FogScattering::LinearClampSampler");
}

void FogScattering::ClearShaderCache()
{
	auto release = [](ID3D11ComputeShader*& a_shader) {
		if (a_shader) {
			a_shader->Release();
			a_shader = nullptr;
		}
	};
	release(prepareCS);
	release(downsampleCS);
	release(compositeCS);
}

ID3D11ComputeShader* FogScattering::GetPrepareCS()
{
	if (!prepareCS)
		prepareCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FogScattering\\FogScatteringPrepareCS.hlsl", {}, "cs_5_0"));
	return prepareCS;
}

ID3D11ComputeShader* FogScattering::GetDownsampleCS()
{
	if (!downsampleCS)
		downsampleCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FogScattering\\FogScatteringDownsampleCS.hlsl", {}, "cs_5_0"));
	return downsampleCS;
}

ID3D11ComputeShader* FogScattering::GetCompositeCS()
{
	if (!compositeCS)
		compositeCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FogScattering\\FogScatteringCompositeCS.hlsl", {}, "cs_5_0"));
	return compositeCS;
}

void FogScattering::ReleaseResources()
{
	mipSRVs.clear();
	mipUAVs.clear();
	scatterPyramid.reset();
	compositeTarget.reset();
	scatterWidth = 0;
	scatterHeight = 0;
	mipCount = 0;
}

void FogScattering::EnsureResources(uint32_t a_width, uint32_t a_height, ID3D11Texture2D* a_sceneTexture)
{
	if (scatterPyramid && compositeTarget && scatterWidth == a_width && scatterHeight == a_height)
		return;

	ReleaseResources();

	uint32_t levels = 1;
	while (levels < kMaxMipCount &&
			(a_width >> levels) >= kMinMipExtent &&
			(a_height >> levels) >= kMinMipExtent)
		levels++;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_width;
	desc.Height = a_height;
	desc.MipLevels = levels;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	scatterPyramid = std::make_unique<Texture2D>(desc, "FogScattering::ScatterPyramid");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = levels;
	scatterPyramid->CreateSRV(srvDesc);

	auto device = globals::d3d::device;
	mipSRVs.resize(levels);
	mipUAVs.resize(levels);
	for (uint32_t mip = 0; mip < levels; mip++) {
		D3D11_SHADER_RESOURCE_VIEW_DESC mipSRVDesc = srvDesc;
		mipSRVDesc.Texture2D.MostDetailedMip = mip;
		mipSRVDesc.Texture2D.MipLevels = 1;
		DX::ThrowIfFailed(device->CreateShaderResourceView(scatterPyramid->resource.get(), &mipSRVDesc, mipSRVs[mip].put()));
		Util::SetResourceName(mipSRVs[mip].get(), std::format("FogScattering::ScatterPyramid Mip{} SRV", mip).c_str());

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = mip;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(scatterPyramid->resource.get(), &uavDesc, mipUAVs[mip].put()));
		Util::SetResourceName(mipUAVs[mip].get(), std::format("FogScattering::ScatterPyramid Mip{} UAV", mip).c_str());
	}

	D3D11_TEXTURE2D_DESC sceneDesc{};
	a_sceneTexture->GetDesc(&sceneDesc);

	D3D11_TEXTURE2D_DESC compositeDesc{};
	compositeDesc.Width = sceneDesc.Width;
	compositeDesc.Height = sceneDesc.Height;
	compositeDesc.MipLevels = 1;
	compositeDesc.ArraySize = 1;
	compositeDesc.Format = sceneDesc.Format;
	compositeDesc.SampleDesc.Count = 1;
	compositeDesc.Usage = D3D11_USAGE_DEFAULT;
	compositeDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

	compositeTarget = std::make_unique<Texture2D>(compositeDesc, "FogScattering::CompositeTarget");

	D3D11_UNORDERED_ACCESS_VIEW_DESC compositeUAVDesc{};
	compositeUAVDesc.Format = compositeDesc.Format;
	compositeUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	compositeUAVDesc.Texture2D.MipSlice = 0;
	compositeTarget->CreateUAV(compositeUAVDesc);

	scatterWidth = a_width;
	scatterHeight = a_height;
	mipCount = levels;
}

bool FogScattering::ResolveFogParams(float4& o_fogParams) const
{
	float fogNear = settings.ManualFogNear;
	float fogFar = settings.ManualFogFar;
	float fogPower = settings.ManualFogPower;
	float fogClamp = settings.ManualFogClamp;

	if (!settings.UseManualFog) {
		auto sky = RE::Sky::GetSingleton();
		if (!sky)
			return false;
		fogNear = sky->fogNear;
		fogFar = sky->fogFar;
		fogPower = sky->fogPower;
		fogClamp = sky->fogClamp;
	}

	if (!std::isfinite(fogNear) || !std::isfinite(fogFar) || !std::isfinite(fogPower) || !std::isfinite(fogClamp))
		return false;

	fogClamp = std::clamp(fogClamp, 0.0f, 1.0f);
	if (fogClamp <= 0.0f)
		return false;
	if (fogFar <= fogNear + 1.0f)
		return false;

	o_fogParams = {
		std::max(fogNear, 0.0f),
		1.0f / (fogFar - fogNear),
		std::max(fogPower, 0.05f),
		fogClamp
	};
	return true;
}

void FogScattering::ApplyScattering(RE::RENDER_TARGET a_input)
{
	if (!settings.Enable || settings.Intensity <= 0.0f)
		return;

	auto state = globals::state;
	if (!state || state->isMainMenuOpen || state->isLoadingMenuOpen)
		return;

	// The tonemap thunk is installed at two call sites; only scatter once per frame.
	if (lastAppliedFrame == state->frameCount)
		return;
	lastAppliedFrame = state->frameCount;

	if (!ResolveFogParams(resolvedFogParams))
		return;

	auto renderer = globals::game::renderer;
	auto shadowState = globals::game::shadowState;
	auto graphicsState = globals::game::graphicsState;
	if (!renderer || !shadowState || !graphicsState)
		return;

	auto& sceneRT = renderer->GetRuntimeData().renderTargets[a_input];
	if (!sceneRT.SRV || !sceneRT.UAV)
		return;

	auto* depthSRV = Util::GetCurrentSceneDepthSRV();
	if (!depthSRV)
		return;

	float2 screenSize{ (float)graphicsState->screenWidth, (float)graphicsState->screenHeight };
	auto renderSize = Util::ConvertToDynamic(screenSize);
	uint32_t fullWidth = std::max(1u, (uint32_t)renderSize.x);
	uint32_t fullHeight = std::max(1u, (uint32_t)renderSize.y);
	uint32_t halfWidth = std::max(1u, fullWidth / 2);
	uint32_t halfHeight = std::max(1u, fullHeight / 2);

	EnsureResources(halfWidth, halfHeight, sceneRT.texture);
	if (!scatterPyramid || !compositeTarget || mipCount == 0)
		return;

	auto* prepare = GetPrepareCS();
	auto* downsample = GetDownsampleCS();
	auto* composite = GetCompositeCS();
	if (!prepare || !downsample || !composite)
		return;

	auto eye = shadowState->GetRuntimeData().cameraData.getEye();
	float projX = eye.projMat(0, 0);
	float projY = eye.projMat(1, 1);
	if (projX == 0.0f || projY == 0.0f)
		return;

	FogScatteringCB data{};
	data.BufferDim = { (float)fullWidth, (float)fullHeight, 1.0f / (float)fullWidth, 1.0f / (float)fullHeight };
	data.ScatterDim = { (float)halfWidth, (float)halfHeight, 1.0f / (float)halfWidth, 1.0f / (float)halfHeight };
	data.CameraData = Util::GetCameraData();
	data.FogParams = resolvedFogParams;
	data.ViewRayParams = { 1.0f / projX, 1.0f / projY, 0.0f, 0.0f };
	data.ScatterParams = { settings.Intensity, settings.Radius * (float)(mipCount - 1), std::clamp(settings.DensityThreshold, 0.0f, 0.95f), 0.0f };
	data.ScatterParams2 = { std::max(settings.MaxLuminance, 1.0f), 0.0f, 0.0f, 0.0f };
	fogScatteringCB->Update(&data, sizeof(data));

	auto context = globals::d3d::context;

	ID3D11Buffer* cb = fogScatteringCB->CB();
	ID3D11SamplerState* sampler = linearClampSampler.get();
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetSamplers(0, 1, &sampler);

	ID3D11ShaderResourceView* nullSRVs[3]{ nullptr, nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUAV[1]{ nullptr };

	{
		ID3D11ShaderResourceView* srvs[2]{ sceneRT.SRV, depthSRV };
		ID3D11UnorderedAccessView* uavs[1]{ mipUAVs[0].get() };
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(prepare, nullptr, 0);
		context->Dispatch((halfWidth + 7) / 8, (halfHeight + 7) / 8, 1);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetShaderResources(0, 2, nullSRVs);
	}

	context->CSSetShader(downsample, nullptr, 0);
	for (uint32_t mip = 1; mip < mipCount; mip++) {
		uint32_t mipWidth = std::max(1u, halfWidth >> mip);
		uint32_t mipHeight = std::max(1u, halfHeight >> mip);

		ID3D11ShaderResourceView* srvs[1]{ mipSRVs[mip - 1].get() };
		ID3D11UnorderedAccessView* uavs[1]{ mipUAVs[mip].get() };
		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->Dispatch((mipWidth + 7) / 8, (mipHeight + 7) / 8, 1);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetShaderResources(0, 1, nullSRVs);
	}

	{
		ID3D11ShaderResourceView* srvs[3]{ scatterPyramid->srv.get(), depthSRV, sceneRT.SRV };
		ID3D11UnorderedAccessView* uavs[1]{ compositeTarget->uav.get() };
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(composite, nullptr, 0);
		context->Dispatch((fullWidth + 7) / 8, (fullHeight + 7) / 8, 1);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetShaderResources(0, 3, nullSRVs);
	}

	D3D11_BOX copyBox{ 0, 0, 0, fullWidth, fullHeight, 1 };
	context->CopySubresourceRegion(sceneRT.texture, 0, 0, 0, 0, compositeTarget->resource.get(), 0, &copyBox);

	ID3D11Buffer* nullCB[1]{ nullptr };
	ID3D11SamplerState* nullSampler[1]{ nullptr };
	context->CSSetShader(nullptr, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, nullCB);
	context->CSSetSamplers(0, 1, nullSampler);
}
