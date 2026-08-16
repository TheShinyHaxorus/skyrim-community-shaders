#include "SpecularAntialiasing.h"

#include "I18n/I18n.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.specular_aa."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SpecularAntialiasing::Settings,
	Enable,
	Variance,
	Threshold)

SpecularAntialiasing::PerFrame SpecularAntialiasing::GetCommonBufferData() const
{
	PerFrame data{};
	data.Enable = loaded && settings.Enable;
	data.Variance = settings.Variance;
	data.Threshold = settings.Threshold;
	return data;
}

void SpecularAntialiasing::RestoreDefaultSettings()
{
	settings = {};
}

void SpecularAntialiasing::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SpecularAntialiasing::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SpecularAntialiasing::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable"), "Enable Specular Antialiasing"), &settings.Enable);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"),
							  "Widens roughness where the shading normal varies quickly across a pixel, so sub-pixel "
							  "detail turns into a broader highlight rather than a flickering one."));
	}

	ImGui::BeginDisabled(!settings.Enable);

	ImGui::SliderFloat(T(TKEY("variance"), "Strength"), &settings.Variance, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("variance_tooltip"),
							  "How much of the measured normal variance is folded into roughness. Raise it if distant "
							  "armour and snow still shimmer, lower it if surfaces look dull."));
	}

	ImGui::SliderFloat(T(TKEY("threshold"), "Maximum Roughening"), &settings.Threshold, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("threshold_tooltip"),
							  "Upper bound on the added roughness. Keeps silhouettes and creases, where the normal "
							  "changes fastest, from washing out completely."));
	}

	ImGui::EndDisabled();
}

#undef I18N_KEY_PREFIX
