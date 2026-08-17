#pragma once

struct SkyGradient : Feature
{
	virtual inline std::string GetName() override { return "Sky Gradient"; }
	virtual std::string GetDisplayName() override { return T("feature.sky_gradient.name", "Sky Gradient"); }
	virtual inline std::string GetShortName() override { return "SkyGradient"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.sky_gradient.description", "Recombines the weather's horizon, middle and zenith sky colours per pixel in a perceptual colour space, so the bands where they meet blend smoothly instead of passing through grey."),
			{ T("feature.sky_gradient.key_feature_1", "Removes the muddy desaturated band between a warm horizon and a blue zenith"),
				T("feature.sky_gradient.key_feature_2", "Blends per pixel rather than per sky-dome vertex, so the mesh stops faceting the gradient"),
				T("feature.sky_gradient.key_feature_3", "Keeps the weather's authored gradient shape; only the mixing changes"),
				T("feature.sky_gradient.key_feature_4", "A few instructions in the sky shader; no extra passes or memory") } };
	};

	struct Settings
	{
		bool Enable = true;
		float Strength = 1.0f;
		float ChromaPreservation = 0.75f;
		float Smoothing = 0.5f;
		float DitherStrength = 1.0f;
	};

	Settings settings;

	struct alignas(16) PerFrame
	{
		uint32_t Enable;
		float Strength;
		float ChromaPreservation;
		float Smoothing;

		float DitherStrength;
		float pad0[3];
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	PerFrame GetCommonBufferData() const;

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
};
