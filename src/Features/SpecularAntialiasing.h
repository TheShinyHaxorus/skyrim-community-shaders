#pragma once

/** @brief Widens roughness by the per-pixel normal variance so minified normal maps stop aliasing into specular sparkle. */
struct SpecularAntialiasing : Feature
{
	virtual inline std::string GetName() override { return "Specular Antialiasing"; }
	virtual std::string GetDisplayName() override { return T("feature.specular_aa.name", "Specular Antialiasing"); }
	virtual inline std::string GetShortName() override { return "SpecularAntialiasing"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kMaterials; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.specular_aa.description", "Removes the crawling specular sparkle on distant detailed surfaces by widening roughness with the normal variance inside each pixel, instead of leaving temporal antialiasing to smear it."),
			{ T("feature.specular_aa.key_feature_1", "Stops shimmer on distant armour, chainmail, snow and wet rock"),
				T("feature.specular_aa.key_feature_2", "Filters the roughness written to the G-buffer, so deferred and cubemap reflections are stabilised too"),
				T("feature.specular_aa.key_feature_3", "A handful of shader instructions; no extra passes, textures or memory") } };
	};

	struct Settings
	{
		bool Enable = true;
		float Variance = 0.25f;
		float Threshold = 0.20f;
	};

	Settings settings;

	// Mirrors SpecularAASettings in Common/SharedData.hlsli (FeatureData cbuffer, b6).
	struct alignas(16) PerFrame
	{
		uint32_t Enable;
		float Variance;
		float Threshold;
		uint32_t pad;
	};

	PerFrame GetCommonBufferData() const;

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
};
