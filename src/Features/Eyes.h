#pragma once

#include "I18n/I18n.h"

struct Eyes : Feature
{
	virtual inline std::string GetName() override { return "PBR Eyes"; }
	virtual inline std::string GetDisplayName() override { return T("feature.eyes.name", "PBR Eyes"); }
	virtual inline std::string GetShortName() override { return "Eyes"; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }
	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.eyes.description", "Physically based eye shading using the analytic corneal normal."),
			{ T("feature.eyes.key_feature_1", "Sharp corneal specular highlight from the eyeball sphere normal"),
				T("feature.eyes.key_feature_2", "Refracted iris parallax through the corneal bulge"),
				T("feature.eyes.key_feature_3", "Energy conserving clear coat over the iris and sclera"),
				T("feature.eyes.key_feature_4", "Corneal reflections that track the eyeball, not the normal map") }
		};
	}

	virtual inline std::string_view GetShaderDefineName() override { return "CS_EYES"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	struct alignas(16) Settings
	{
		uint Enable = true;
		float CorneaRoughness = 0.08f;
		float CorneaF0 = 0.027f;
		float CorneaNormalStrength = 1.0f;
		float IrisDepth = 0.04f;
		float IrisIOR = 1.376f;
		uint UseEnvMaskAsCorneaMask = false;
		uint EnableCorneaReflection = true;
	} settings;
};
