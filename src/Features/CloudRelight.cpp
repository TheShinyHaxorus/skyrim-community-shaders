#include "CloudRelight.h"

#include "CloudShadows.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.cloud_relight."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudRelight::Settings,
	enabled,
	cloudRelightMix,
	cloudOriginalMix,
	silverLiningMix,
	silverLiningSpread)

void CloudRelight::DataLoaded()
{
	if (!globals::features::cloudShadows.loaded) {
		failedLoadedMessage = "Cloud Shadows is required for Cloud Relight to function.";
		loaded = false;
	}
}

void CloudRelight::RestoreDefaultSettings()
{
	settings = {};
}

void CloudRelight::LoadSettings(json& o_json)
{
	settings = o_json;
}

void CloudRelight::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CloudRelight::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), reinterpret_cast<bool*>(&settings.enabled));

	ImGui::SeparatorText(T(TKEY("cloud_relighting"), "Cloud Relighting"));

	ImGui::SliderFloat(T(TKEY("vanilla_mix"), "Vanilla Mix"), &settings.cloudOriginalMix, 0.f, 2.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("vanilla_mix_tooltip"), "Multiplier on the original (vanilla) cloud color before relighting is applied."));
	}

	ImGui::SliderFloat(T(TKEY("relight_mix"), "Relight Mix"), &settings.cloudRelightMix, 0.f, 2.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("relight_mix_tooltip"), "Multiplier on the physically-based directional light contribution added to clouds."));
	}

	ImGui::SeparatorText(T(TKEY("silver_lining"), "Silver Lining"));

	ImGui::SliderFloat(T(TKEY("silver_lining_accent"), "Silver Lining Accent"), &settings.silverLiningMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("silver_lining_accent_tooltip"), "Blend between flat isotropic phase (0) and the sharp silver-lining phase function (1)."));
	}

	ImGui::SliderFloat(T(TKEY("silver_lining_spread"), "Silver Lining Spread"), &settings.silverLiningSpread, -0.99f, 0.99f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("silver_lining_spread_tooltip"), "Positive: silver-lining appears only on thin cloud edges.\nNegative: silver-lining bleeds into thick cloud areas."));
	}
}

#undef I18N_KEY_PREFIX
