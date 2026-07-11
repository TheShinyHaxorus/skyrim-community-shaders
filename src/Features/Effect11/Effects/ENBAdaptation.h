#pragma once

#include "ExtendedEffect.h"

class ENBAdaptation : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbadaptation.fx"; }

	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	TextureManager::Texture textureCurrent;

protected:
	void CreateEffectTextures() override;

private:
	void EnsureNeutralTexture(float value);

	uint32_t idForceMinMax = 0xFFFFFFFF;
	uint32_t idAdaptTime = 0xFFFFFFFF;
	uint32_t idAdaptMin = 0xFFFFFFFF;
	uint32_t idAdaptMax = 0xFFFFFFFF;
	uint32_t idAdaptSens = 0xFFFFFFFF;
	uint32_t idNeutralLuminance = 0xFFFFFFFF;
	bool idsCached = false;

	winrt::com_ptr<ID3D11Texture2D> neutralTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> neutralSRV;
	float bakedNeutralLuminance = -1.0f;
};