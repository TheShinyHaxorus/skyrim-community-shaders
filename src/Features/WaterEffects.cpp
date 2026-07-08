#include "WaterEffects.h"

#include "../I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <DDSTextureLoader.h>
#include <algorithm>
#include <cmath>

#define I18N_KEY_PREFIX "feature.water_effects."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WaterEffects::Settings,
	EnableUnderwaterDistortion,
	BlurAmount,
	BlurRange,
	DistortionAmount,
	DistortionScale,
	DistortionSpeed,
	VerticalAmount,
	NoiseScale,
	TransitionRange)

void WaterEffects::SetupResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\WaterEffects\\watercaustics.dds", nullptr, causticsView.put());

	underwaterCB = std::make_unique<ConstantBuffer>(
		CD3D11_BUFFER_DESC(sizeof(UnderwaterCB), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE),
		"WaterEffects::UnderwaterCB");

	{
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		device->CreateSamplerState(&desc, linearClampSampler.put());
		Util::SetResourceName(linearClampSampler.get(), "WaterEffects::LinearClampSampler");

		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		device->CreateSamplerState(&desc, pointClampSampler.put());
		Util::SetResourceName(pointClampSampler.get(), "WaterEffects::PointClampSampler");
	}

	{
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.DepthClipEnable = TRUE;
		desc.ScissorEnable = FALSE;
		device->CreateRasterizerState(&desc, rasterizerState.put());
		Util::SetResourceName(rasterizerState.get(), "WaterEffects::RasterizerState");
	}

	{
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = FALSE;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		device->CreateBlendState(&desc, opaqueBlendState.put());
		Util::SetResourceName(opaqueBlendState.get(), "WaterEffects::OpaqueBlendState");
	}

	{
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = FALSE;
		desc.StencilEnable = FALSE;
		device->CreateDepthStencilState(&desc, noDepthState.put());
		Util::SetResourceName(noDepthState.get(), "WaterEffects::NoDepthState");
	}
}

void WaterEffects::Reset()
{
	sceneColorCopy = nullptr;
	blurTargetA = nullptr;
	blurTargetB = nullptr;
	targetWidth = 0;
	targetHeight = 0;
	targetFormat = DXGI_FORMAT_UNKNOWN;
}

void WaterEffects::Prepass()
{
	auto context = globals::d3d::context;
	auto srv = causticsView.get();
	context->PSSetShaderResources(65, 1, &srv);
}

bool WaterEffects::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

ID3D11VertexShader* WaterEffects::GetUnderwaterVS()
{
	if (!underwaterVS) {
		underwaterVS = reinterpret_cast<ID3D11VertexShader*>(
			Util::CompileShader(L"Data/Shaders/WaterEffects/UnderwaterDistortion.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}
	return underwaterVS;
}

ID3D11PixelShader* WaterEffects::GetUnderwaterBlurPS()
{
	if (!underwaterBlurPS) {
		underwaterBlurPS = reinterpret_cast<ID3D11PixelShader*>(
			Util::CompileShader(L"Data/Shaders/WaterEffects/UnderwaterDistortion.hlsl", { { "PSHADER", "" }, { "PASS_BLUR", "" } }, "ps_5_0"));
	}
	return underwaterBlurPS;
}

ID3D11PixelShader* WaterEffects::GetUnderwaterDrawPS()
{
	if (!underwaterDrawPS) {
		underwaterDrawPS = reinterpret_cast<ID3D11PixelShader*>(
			Util::CompileShader(L"Data/Shaders/WaterEffects/UnderwaterDistortion.hlsl", { { "PSHADER", "" }, { "PASS_DRAW", "" } }, "ps_5_0"));
	}
	return underwaterDrawPS;
}

void WaterEffects::ClearShaderCache()
{
	if (underwaterVS) {
		underwaterVS->Release();
		underwaterVS = nullptr;
	}
	if (underwaterBlurPS) {
		underwaterBlurPS->Release();
		underwaterBlurPS = nullptr;
	}
	if (underwaterDrawPS) {
		underwaterDrawPS->Release();
		underwaterDrawPS = nullptr;
	}
}

bool WaterEffects::EnsureUnderwaterTargets(const D3D11_TEXTURE2D_DESC& a_outputDesc, DXGI_FORMAT a_srvFormat, DXGI_FORMAT a_rtvFormat)
{
	if (sceneColorCopy && blurTargetA && blurTargetB &&
		targetWidth == a_outputDesc.Width && targetHeight == a_outputDesc.Height && targetFormat == a_outputDesc.Format) {
		return true;
	}

	sceneColorCopy = nullptr;
	blurTargetA = nullptr;
	blurTargetB = nullptr;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_outputDesc.Width;
	desc.Height = a_outputDesc.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = a_outputDesc.Format;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = a_srvFormat;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = a_rtvFormat;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Texture2D.MipSlice = 0;

	try {
		sceneColorCopy = std::make_unique<Texture2D>(desc, "WaterEffects::UnderwaterSceneColor");
		sceneColorCopy->CreateSRV(srvDesc);

		blurTargetA = std::make_unique<Texture2D>(desc, "WaterEffects::UnderwaterBlurA");
		blurTargetA->CreateSRV(srvDesc);
		blurTargetA->CreateRTV(rtvDesc);

		blurTargetB = std::make_unique<Texture2D>(desc, "WaterEffects::UnderwaterBlurB");
		blurTargetB->CreateSRV(srvDesc);
		blurTargetB->CreateRTV(rtvDesc);
	} catch (const std::exception&) {
		sceneColorCopy = nullptr;
		blurTargetA = nullptr;
		blurTargetB = nullptr;
		return false;
	}

	targetWidth = a_outputDesc.Width;
	targetHeight = a_outputDesc.Height;
	targetFormat = a_outputDesc.Format;
	return true;
}

void WaterEffects::DrawUnderwater(RE::RENDER_TARGET a_output)
{
	if (!loaded || !settings.EnableUnderwaterDistortion)
		return;

	const float submergence = Util::TryGetWaterData(0.0f, 0.0f).w;
	const float range = std::max(settings.TransitionRange, 1.0f);
	const float strength = std::clamp(submergence / range, 0.0f, 1.0f);
	if (strength <= 0.0f)
		return;

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	if (!renderer || !context)
		return;

	auto& output = renderer->GetRuntimeData().renderTargets[a_output];
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!output.texture || !output.RTV || !mainDepth.depthSRV)
		return;

	auto* vs = GetUnderwaterVS();
	auto* blurPS = GetUnderwaterBlurPS();
	auto* drawPS = GetUnderwaterDrawPS();
	if (!vs || !blurPS || !drawPS)
		return;

	D3D11_TEXTURE2D_DESC outputDesc{};
	output.texture->GetDesc(&outputDesc);

	DXGI_FORMAT srvFormat = outputDesc.Format;
	DXGI_FORMAT rtvFormat = outputDesc.Format;
	if (output.SRV) {
		D3D11_SHADER_RESOURCE_VIEW_DESC d{};
		output.SRV->GetDesc(&d);
		srvFormat = d.Format;
	}
	{
		D3D11_RENDER_TARGET_VIEW_DESC d{};
		output.RTV->GetDesc(&d);
		rtvFormat = d.Format;
	}

	if (!EnsureUnderwaterTargets(outputDesc, srvFormat, rtvFormat))
		return;

	auto state = globals::state;
	state->BeginPerfEvent("WaterEffects::Underwater");

	// Unbind the output target (still bound as RTV from tonemap) before copying from it.
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	context->CopyResource(sceneColorCopy->resource.get(), output.texture);

	auto viewport = globals::game::graphicsState;
	const float widthRatio = viewport->GetRuntimeData().dynamicResolutionWidthRatio;
	const float heightRatio = viewport->GetRuntimeData().dynamicResolutionHeightRatio;

	UnderwaterCB cbData{};
	cbData.Params0 = { settings.BlurAmount, settings.BlurRange, settings.DistortionAmount, settings.DistortionScale };
	cbData.Params1 = { settings.DistortionSpeed, strength, settings.NoiseScale, std::fmod(state->timer, 16777.216f) / 16777.216f };
	cbData.ScreenParams = { 1.0f / (float)outputDesc.Width, 1.0f / (float)outputDesc.Height, settings.VerticalAmount, 0.0f };
	cbData.DynRes = { widthRatio, heightRatio, 0.0f, 0.0f };
	underwaterCB->Update(cbData);

	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vs, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);

	D3D11_VIEWPORT vp{};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = (float)outputDesc.Width;
	vp.Height = (float)outputDesc.Height;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	context->RSSetViewports(1, &vp);
	context->RSSetState(rasterizerState.get());
	context->OMSetBlendState(opaqueBlendState.get(), nullptr, 0xffffffff);
	context->OMSetDepthStencilState(noDepthState.get(), 0);

	ID3D11SamplerState* samplers[] = { linearClampSampler.get(), pointClampSampler.get() };
	context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	ID3D11Buffer* cb = underwaterCB->CB();
	context->PSSetConstantBuffers(0, 1, &cb);

	ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };

	// Pass 1: depth-weighted distort/blur of the scene color into blur target A.
	{
		ID3D11RenderTargetView* rtv = blurTargetA->rtv.get();
		context->OMSetRenderTargets(1, &rtv, nullptr);
		ID3D11ShaderResourceView* srvs[] = { sceneColorCopy->srv.get(), mainDepth.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		context->PSSetShader(blurPS, nullptr, 0);
		context->Draw(3, 0);
		context->PSSetShaderResources(0, ARRAYSIZE(nullSRVs), nullSRVs);
	}

	// Pass 2: second distort/blur pass, ping-ponging A into blur target B.
	{
		ID3D11RenderTargetView* rtv = blurTargetB->rtv.get();
		context->OMSetRenderTargets(1, &rtv, nullptr);
		ID3D11ShaderResourceView* srvs[] = { blurTargetA->srv.get(), mainDepth.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		context->PSSetShader(blurPS, nullptr, 0);
		context->Draw(3, 0);
		context->PSSetShaderResources(0, ARRAYSIZE(nullSRVs), nullSRVs);
	}

	// Pass 3: composite the distorted color over the original into the output target.
	{
		context->OMSetRenderTargets(1, &output.RTV, nullptr);
		ID3D11ShaderResourceView* srvs[] = { blurTargetB->srv.get(), sceneColorCopy->srv.get() };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		context->PSSetShader(drawPS, nullptr, 0);
		context->Draw(3, 0);
		context->PSSetShaderResources(0, ARRAYSIZE(nullSRVs), nullSRVs);
	}

	state->EndPerfEvent();
}

void WaterEffects::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("underwater_enable"), "Underwater Distortion"), &settings.EnableUnderwaterDistortion);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_enable_tooltip"),
							  "Applies an ENB-style refractive distortion and depth blur to the screen while the camera is underwater."));
	}

	if (!settings.EnableUnderwaterDistortion)
		return;

	ImGui::SliderFloat(T(TKEY("underwater_blur_amount"), "Blur Amount"), &settings.BlurAmount, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_blur_amount_tooltip"),
							  "How strongly the underwater blur is mixed with the sharp image."));
	}

	ImGui::SliderFloat(T(TKEY("underwater_blur_range"), "Blur Range"), &settings.BlurRange, 0.0f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_blur_range_tooltip"),
							  "Screen-space radius of the underwater blur taps."));
	}

	ImGui::SliderFloat(T(TKEY("underwater_distortion_amount"), "Distortion Amount"), &settings.DistortionAmount, 0.0f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_distortion_amount_tooltip"),
							  "Strength of the refractive wobble."));
	}

	ImGui::SliderFloat(T(TKEY("underwater_distortion_scale"), "Distortion Scale"), &settings.DistortionScale, 1.0f, 64.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_distortion_scale_tooltip"),
							  "Spatial frequency of the refractive wobble."));
	}

	ImGui::SliderFloat(T(TKEY("underwater_distortion_speed"), "Distortion Speed"), &settings.DistortionSpeed, 0.0f, 32.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_distortion_speed_tooltip"),
							  "Animation speed of the refractive wobble."));
	}

	ImGui::SliderFloat(T(TKEY("underwater_vertical_amount"), "Vertical Amount"), &settings.VerticalAmount, 0.0f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_vertical_amount_tooltip"),
							  "Scales the vertical component of the distortion and the surface-line wobble."));
	}

	ImGui::SliderFloat(T(TKEY("underwater_noise_scale"), "Surface Wobble Scale"), &settings.NoiseScale, 1.0f, 32.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_noise_scale_tooltip"),
							  "Frequency of the animated surface-line wobble noise."));
	}

	ImGui::SliderFloat(T(TKEY("underwater_transition_range"), "Transition Depth"), &settings.TransitionRange, 1.0f, 256.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("underwater_transition_range_tooltip"),
							  "Submersion depth (game units) over which the effect fades to full strength."));
	}
}

void WaterEffects::LoadSettings(json& o_json)
{
	settings = o_json;
}

void WaterEffects::SaveSettings(json& o_json)
{
	o_json = settings;
}

void WaterEffects::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
