#include "ENBAdaptation.h"

#include "../SettingManager.h"
#include "../TextureManager.h"

#include "Utils/D3D.h"

void ENBAdaptation::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto ui = globals::game::ui;
	if (ui && ui->IsMenuOpen(RE::StatsMenu::MENU_NAME) && EnsureNeutralTexture()) {
		SetShaderResourceVariable("TextureCurrent", textureNeutralSRV.get());
	} else {
		auto* currentSRV = textureManager.GetDownsampleTextureBlurry();
		if (!currentSRV) {
			return;
		}

		SetShaderResourceVariable("TextureCurrent", currentSRV);

		if (!textureCurrent.texture)
			return;

		ExecuteTechnique("Downsample", textureCurrent);

		SetShaderResourceVariable("TextureCurrent", textureCurrent.srv.get());
	}

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

bool ENBAdaptation::EnsureNeutralTexture()
{
	if (textureNeutralSRV)
		return true;

	auto device = globals::d3d::device;
	if (!device)
		return false;

	// Average-scene luminance the perk menu adapts to; the preset's own
	// sensitivity/min/max settings still shape the final brightness.
	constexpr float kNeutralLuminance = 0.18f;
	constexpr uint32_t kSize = 16;

	std::array<float, kSize * kSize> texels;
	texels.fill(kNeutralLuminance);

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = kSize;
	desc.Height = kSize;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data{};
	data.pSysMem = texels.data();
	data.SysMemPitch = kSize * sizeof(float);

	if (FAILED(device->CreateTexture2D(&desc, &data, textureNeutral.put())))
		return false;
	Util::SetResourceName(textureNeutral.get(), "ENBAdaptation::TextureNeutral");

	if (FAILED(device->CreateShaderResourceView(textureNeutral.get(), nullptr, textureNeutralSRV.put()))) {
		textureNeutral = nullptr;
		return false;
	}
	Util::SetResourceName(textureNeutralSRV.get(), "ENBAdaptation::TextureNeutral SRV");
	return true;
}