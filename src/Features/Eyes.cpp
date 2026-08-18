#include "Eyes.h"

#include "I18n/I18n.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Eyes::Settings,
	Enable,
	CorneaRoughness,
	CorneaF0,
	CorneaNormalStrength,
	IrisDepth,
	IrisIOR,
	UseEnvMaskAsCorneaMask,
	EnableCorneaReflection)

void Eyes::RestoreDefaultSettings()
{
	settings = {};
}

void Eyes::LoadSettings(json& o_json)
{
	settings = o_json;
}

void Eyes::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Eyes::DrawSettings()
{
	ImGui::Checkbox(T("feature.eyes.enable", "Enable PBR Eyes"), reinterpret_cast<bool*>(&settings.Enable));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.enable_tooltip", "Shades eyes as a clear corneal layer over the iris and sclera, using the analytic eyeball normal that the vanilla shader computes but discards."));
	}

	ImGui::BeginDisabled(!settings.Enable);

	ImGui::SeparatorText(T("feature.eyes.cornea_section", "Cornea"));

	ImGui::SliderFloat(T("feature.eyes.cornea_roughness", "Cornea Roughness"), &settings.CorneaRoughness, 0.02f, 0.5f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_roughness_tooltip", "Roughness of the tear film. Lower values give a tighter, brighter catchlight."));
	}

	ImGui::SliderFloat(T("feature.eyes.cornea_f0", "Cornea Reflectance"), &settings.CorneaF0, 0.0f, 0.1f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_f0_tooltip", "Normal incidence reflectance of the cornea. 0.027 matches an index of refraction of 1.376."));
	}

	ImGui::SliderFloat(T("feature.eyes.cornea_normal_strength", "Cornea Normal Strength"), &settings.CorneaNormalStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_normal_strength_tooltip", "Blends the specular normal from the normal mapped surface toward the analytic eyeball sphere normal."));
		ImGui::BulletText("%s", T("feature.eyes.cornea_normal_strength_bullet", "The diffuse layer always keeps the normal mapped surface."));
	}

	ImGui::Checkbox(T("feature.eyes.cornea_reflection", "Corneal Reflections"), reinterpret_cast<bool*>(&settings.EnableCorneaReflection));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_reflection_tooltip", "Samples environment and cubemap reflections along the eyeball normal instead of the normal mapped surface."));
	}

	ImGui::SeparatorText(T("feature.eyes.iris_section", "Iris"));

	ImGui::SliderFloat(T("feature.eyes.iris_depth", "Iris Depth"), &settings.IrisDepth, 0.0f, 0.15f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.iris_depth_tooltip", "Distance from the cornea to the iris plane, in texture coordinates. Drives the parallax as the view angle changes."));
	}

	ImGui::SliderFloat(T("feature.eyes.iris_ior", "Aqueous Humour IOR"), &settings.IrisIOR, 1.0f, 1.6f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.iris_ior_tooltip", "Index of refraction of the fluid between the cornea and the iris. Higher values bend the view ray more and shrink the parallax."));
	}

	ImGui::Checkbox(T("feature.eyes.use_env_mask", "Cornea Mask From Environment Mask"), reinterpret_cast<bool*>(&settings.UseEnvMaskAsCorneaMask));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.use_env_mask_tooltip", "Restricts the effect to the environment mask of the eye. Off by default, which covers the whole eyeball in tear film."));
		ImGui::BulletText("%s", T("feature.eyes.use_env_mask_bullet", "Most eye assets author this map as a faint reflection strength mask rather than a corneal disc, so enabling it usually removes the effect. It is ignored on meshes with no environment mask."));
	}

	ImGui::EndDisabled();
}
