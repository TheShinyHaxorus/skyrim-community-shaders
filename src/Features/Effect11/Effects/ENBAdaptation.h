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
	winrt::com_ptr<ID3D11Texture2D> textureNeutral;
	winrt::com_ptr<ID3D11ShaderResourceView> textureNeutralSRV;
	float bakedNeutralLuminance = -1.0f;
	bool EnsureNeutralTexture();
	uint32_t idForceMinMax = 0xFFFFFFFF;
	uint32_t idAdaptTime = 0xFFFFFFFF;
	uint32_t idAdaptMin = 0xFFFFFFFF;
	uint32_t idAdaptMax = 0xFFFFFFFF;
	uint32_t idAdaptSens = 0xFFFFFFFF;
	bool idsCached = false;
};