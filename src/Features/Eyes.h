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
			T("feature.eyes.description", "Shades eyes as a refractive cornea over the iris, following the eye models used by Unreal Engine and Unity HDRP."),
			{ T("feature.eyes.key_feature_1", "Tear film clear coat driven by the smooth eyeball normal instead of the iris normal map"),
				T("feature.eyes.key_feature_2", "Iris parallax refracted through the anterior chamber, scaled from the measured curvature of the mesh"),
				T("feature.eyes.key_feature_3", "Separate cornea and sclera gloss taken from the eye environment mask"),
				T("feature.eyes.key_feature_4", "Corneal reflections that replace the vanilla eye cubemap and track the eyeball") }
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
		float CorneaRoughness = 0.06f;
		float ScleraRoughness = 0.25f;
		float CorneaF0 = 0.028f;

		float CorneaSmoothing = 1.0f;
		float CorneaFromEnvMask = 1.0f;
		float CatchlightSize = 0.006f;
		float CatchlightStrength = 1.0f;

		float AnteriorChamberDepth = 0.25f;
		float AqueousIOR = 1.336f;
		float ReflectionStrength = 1.0f;
		uint EnableCorneaReflection = true;
	} settings;
};
