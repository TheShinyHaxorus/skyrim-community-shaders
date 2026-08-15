#pragma once

#include "Feature.h"

struct CloudRelight final : Feature
{
	static CloudRelight* GetSingleton()
	{
		static CloudRelight singleton;
		return &singleton;
	}

	inline std::string GetName() override { return "Cloud Relight"; }
	inline std::string GetShortName() override { return "CloudRelight"; }
	inline std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Relights vanilla cloud textures with the directional sun light and the Cloud Shadows "
			"cubemap, adding silver-lining and forward-scattering.",
			{
				"Silver-lining and forward-scattering on vanilla clouds",
				"Cloud self-shadowing via the Cloud Shadows cubemap",
				"Adjustable vanilla vs. relit blend",
			}
		};
	}

	inline std::string_view GetShaderDefineName() override { return "CLOUD_RELIGHT"; }
	inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	void DataLoaded() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;
	void DrawSettings() override;

	struct alignas(16) Settings
	{
		uint32_t enabled = 1;
		float cloudRelightMix = 1.f;
		float cloudOriginalMix = 0.5f;
		float silverLiningMix = 1.f;

		float silverLiningSpread = 0.f;
		float pad[3] = {};
	} settings;
};
