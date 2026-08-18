#include "Eyes.h"

#include "I18n/I18n.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Eyes::Settings,
	Enable,
	CorneaRoughness,
	ScleraRoughness,
	CorneaF0,
	CorneaSmoothing,
	CorneaFromEnvMask,
	CatchlightSize,
	CatchlightStrength,
	AnteriorChamberDepth,
	AqueousIOR,
	ReflectionStrength,
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
		ImGui::Text("%s", T("feature.eyes.enable_tooltip", "Treats the eye as a wet transparent cornea covering the iris and sclera, the way Unreal Engine and Unity HDRP shade eyes, instead of as an opaque painted surface."));
		ImGui::BulletText("%s", T("feature.eyes.enable_bullet", "Applies to player headparts and to baked NPC face geometry alike."));
	}

	ImGui::BeginDisabled(!settings.Enable);

	ImGui::SeparatorText(T("feature.eyes.catchlight_section", "Catchlight"));

	ImGui::SliderFloat(T("feature.eyes.cornea_roughness", "Cornea Roughness"), &settings.CorneaRoughness, 0.02f, 0.5f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_roughness_tooltip", "Roughness of the tear film over the cornea. Lower values give a tighter, brighter catchlight."));
		ImGui::BulletText("%s", T("feature.eyes.cornea_roughness_bullet", "Widened automatically with distance so the highlight never falls below a pixel and starts to sparkle."));
	}

	ImGui::SliderFloat(T("feature.eyes.sclera_roughness", "Sclera Roughness"), &settings.ScleraRoughness, 0.02f, 1.0f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.sclera_roughness_tooltip", "Roughness of the tear film over the white of the eye, which is far less polished than the cornea."));
		ImGui::BulletText("%s", T("feature.eyes.sclera_roughness_bullet", "The split between the two is read from the eye environment mask, so the sharp highlight stays over the cornea instead of covering the whole eyeball."));
	}

	ImGui::SliderFloat(T("feature.eyes.catchlight_size", "Catchlight Size"), &settings.CatchlightSize, 0.0f, 0.1f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.catchlight_size_tooltip", "Apparent size of the light source reflected in the eye. Spreads the highlight into a disc instead of a point."));
		ImGui::BulletText("%s", T("feature.eyes.catchlight_size_bullet", "Total reflected energy is preserved, so a larger catchlight is correspondingly dimmer."));
	}

	ImGui::SliderFloat(T("feature.eyes.catchlight_strength", "Catchlight Strength"), &settings.CatchlightStrength, 0.0f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.catchlight_strength_tooltip", "Multiplier on the corneal highlight from direct light. 1.0 is physically correct."));
	}

	ImGui::SeparatorText(T("feature.eyes.cornea_section", "Cornea"));

	ImGui::SliderFloat(T("feature.eyes.cornea_f0", "Cornea Reflectance"), &settings.CorneaF0, 0.0f, 0.1f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_f0_tooltip", "Normal incidence reflectance of the tear film. 0.028 matches an index of refraction of 1.376."));
	}

	ImGui::SliderFloat(T("feature.eyes.cornea_smoothing", "Cornea Smoothing"), &settings.CorneaSmoothing, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_smoothing_tooltip", "Blends the reflecting surface from the normal mapped iris toward the smooth eyeball. The cornea is a polished bulge, so the iris relief should not disturb it."));
		ImGui::BulletText("%s", T("feature.eyes.cornea_smoothing_bullet", "The layer underneath always keeps the normal mapped surface, so the iris detail stays visible."));
	}

	ImGui::SliderFloat(T("feature.eyes.cornea_from_env_mask", "Cornea From Environment Mask"), &settings.CorneaFromEnvMask, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_from_env_mask_tooltip", "How strongly the eye environment mask decides where the cornea is. Eye assets author that map as a disc over the iris, which is exactly the corneal bulge."));
		ImGui::BulletText("%s", T("feature.eyes.cornea_from_env_mask_bullet", "At zero the whole eyeball is treated as cornea. Ignored on meshes with no environment mask."));
	}

	ImGui::Checkbox(T("feature.eyes.cornea_reflection", "Corneal Reflections"), reinterpret_cast<bool*>(&settings.EnableCorneaReflection));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.cornea_reflection_tooltip", "Reflects the environment off the eyeball rather than off the iris normal map, and replaces the flat vanilla eye cubemap."));
	}

	ImGui::BeginDisabled(!settings.EnableCorneaReflection);
	ImGui::SliderFloat(T("feature.eyes.reflection_strength", "Reflection Strength"), &settings.ReflectionStrength, 0.0f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.reflection_strength_tooltip", "Multiplier on the environment reflected in the cornea. 1.0 is physically correct and is much dimmer than the vanilla eye envmap."));
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText(T("feature.eyes.iris_section", "Iris"));

	ImGui::SliderFloat(T("feature.eyes.anterior_chamber_depth", "Anterior Chamber Depth"), &settings.AnteriorChamberDepth, 0.0f, 0.6f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.anterior_chamber_depth_tooltip", "Distance from the cornea to the iris, in eyeball radii. A human eye is about 0.25, roughly 3 mm behind a 12 mm eyeball."));
		ImGui::BulletText("%s", T("feature.eyes.anterior_chamber_depth_bullet", "Converted to texture space from the curvature the eye mesh actually has, so the parallax stays correct on any eye mesh or actor scale."));
	}

	ImGui::SliderFloat(T("feature.eyes.aqueous_ior", "Aqueous Humour IOR"), &settings.AqueousIOR, 1.0f, 1.6f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.eyes.aqueous_ior_tooltip", "Index of refraction of the fluid between the cornea and the iris. 1.336 is the measured value; higher values bend the view ray more and shrink the parallax."));
	}

	ImGui::EndDisabled();
}
