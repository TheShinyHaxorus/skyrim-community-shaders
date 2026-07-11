#include "ENBAdaptation.h"

#include "../EffectManager.h"
#include "../SettingManager.h"
#include "../TextureManager.h"
#include "Utils/D3D.h"

void ENBAdaptation::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto* currentSRV = textureManager.GetDownsampleTextureBlurry();
	if (!currentSRV) {
		return;
	}

	// While a full-screen menu is up, measure a constant luminance instead of the scene
	if (EffectManager::GetSingleton().GetMenuNeutralBlend() > 0.0f && idsCached) {
		EnsureNeutralTexture(SettingManager::GetSingleton().GetValue<float>(idNeutralLuminance));
		if (neutralSRV)
			currentSRV = neutralSRV.get();
	}

	SetShaderResourceVariable("TextureCurrent", currentSRV);

	if (!textureCurrent.texture)
		return;

	ExecuteTechnique("Downsample", textureCurrent);

	SetShaderResourceVariable("TextureCurrent", textureCurrent.srv.get());

	// Use swap mechanism to determine input/output
	const char* texturePreviousName = (textureManager.GetTextureSwap() & 1) ? "TextureAdaptationSwap" : "TextureAdaptation";
	const char* textureAdaptationName = (textureManager.GetTextureSwap() & 1) ? "TextureAdaptation" : "TextureAdaptationSwap";

	// Set input texture (previous frame's adaptation value)
	auto* texturePrevious = textureManager.GetCommonTexture(texturePreviousName);
	if (!texturePrevious) {
		return;
	}
	SetShaderResourceVariable("TexturePrevious", texturePrevious->srv.get());

	// Execute adaptation technique, writing to output texture
	auto* textureAdaptation = textureManager.GetCommonTexture(textureAdaptationName);
	if (!textureAdaptation) {
		return;
	}
	ExecuteTechnique("Draw", *textureAdaptation);
}

void ENBAdaptation::UpdateEffectVariables()
{
	auto& settingManager = SettingManager::GetSingleton();

	if (!idsCached) {
		idForceMinMax = settingManager.GetSettingID("ForceMinMaxValues", "ADAPTATION");
		idAdaptTime = settingManager.GetSettingID("AdaptationTime", "ADAPTATION");
		idAdaptMin = settingManager.GetSettingID("AdaptationMin", "ADAPTATION");
		idAdaptMax = settingManager.GetSettingID("AdaptationMax", "ADAPTATION");
		idAdaptSens = settingManager.GetSettingID("AdaptationSensitivity", "ADAPTATION");
		idNeutralLuminance = settingManager.GetSettingID("NeutralLuminance", "MENUS");
		idsCached = true;
	}

	auto forceMinMaxValues = settingManager.GetValue<bool>(idForceMinMax);

	float adaptationTime = settingManager.GetValue<float>(idAdaptTime);
	float deltaTime = (globals::game::deltaTime) ? (*globals::game::deltaTime) : 0.0f;

	float4 adaptationParameters{};
	adaptationParameters.x = !forceMinMaxValues ? 0.0f : settingManager.GetValue<float>(idAdaptMin);
	adaptationParameters.y = !forceMinMaxValues ? 65535.0f : settingManager.GetValue<float>(idAdaptMax);
	adaptationParameters.z = settingManager.GetValue<float>(idAdaptSens);
	adaptationParameters.w = std::clamp((adaptationTime > 0.0f) ? (deltaTime / adaptationTime) : 1.0f, 0.0f, 1.0f);

	SetVectorVariable("AdaptationParameters", &adaptationParameters, sizeof(adaptationParameters));
}

void ENBAdaptation::CreateEffectTextures()
{
	textureCurrent = CreateTexture(16, 16, DXGI_FORMAT_R32_FLOAT, "ENBAdaptation::TextureCurrent");
}

void ENBAdaptation::EnsureNeutralTexture(float value)
{
	if (neutralSRV && value == bakedNeutralLuminance)
		return;

	neutralTexture = nullptr;
	neutralSRV = nullptr;

	constexpr uint32_t size = 16;
	float texels[size * size * 4]{};
	for (uint32_t i = 0; i < size * size; i++) {
		texels[i * 4 + 0] = value;
		texels[i * 4 + 1] = value;
		texels[i * 4 + 2] = value;
		texels[i * 4 + 3] = 1.0f;
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = size;
	desc.Height = size;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA initData{};
	initData.pSysMem = texels;
	initData.SysMemPitch = size * 4 * sizeof(float);

	auto device = globals::d3d::device;
	if (!device || FAILED(device->CreateTexture2D(&desc, &initData, neutralTexture.put())))
		return;
	Util::SetResourceName(neutralTexture.get(), "ENBAdaptation::NeutralLuminance");

	if (FAILED(device->CreateShaderResourceView(neutralTexture.get(), nullptr, neutralSRV.put()))) {
		neutralTexture = nullptr;
		return;
	}
	Util::SetResourceName(neutralSRV.get(), "ENBAdaptation::NeutralLuminance SRV");

	bakedNeutralLuminance = value;
}