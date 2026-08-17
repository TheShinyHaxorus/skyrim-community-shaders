#include "SkyGradient.h"

#include "I18n/I18n.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.sky_gradient."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkyGradient::Settings,
	Enable,
	Strength,
	ChromaPreservation,
	Smoothing,
	DitherStrength)

SkyGradient::PerFrame SkyGradient::GetCommonBufferData() const
{
	PerFrame data{};
	data.Enable = loaded && settings.Enable;
	data.Strength = settings.Strength;
	data.ChromaPreservation = settings.ChromaPreservation;
	data.Smoothing = settings.Smoothing;
	data.DitherStrength = settings.DitherStrength;
	return data;
}

void SkyGradient::RestoreDefaultSettings()
{
	settings = {};
}

void SkyGradient::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SkyGradient::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SkyGradient::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable"), "Enable Sky Gradient"), &settings.Enable);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"),
							  "Mixes the weather's three sky colours per pixel in Oklab instead of per vertex in RGB, "
							  "so the seams between horizon, middle and zenith stop banding."));
	}

	ImGui::BeginDisabled(!settings.Enable);

	ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("strength_tooltip"),
							  "Blend toward the perceptual mix. At 0 the sky is exactly vanilla."));
	}

	ImGui::SliderFloat(T(TKEY("chroma_preservation"), "Chroma Preservation"), &settings.ChromaPreservation, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("chroma_preservation_tooltip"),
							  "Keeps saturation up through the middle of the gradient. Raise it if the band between a "
							  "warm horizon and a blue zenith still looks grey."));
	}

	ImGui::SliderFloat(T(TKEY("smoothing"), "Seam Smoothing"), &settings.Smoothing, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("smoothing_tooltip"),
							  "Softens the straight ramps the sky dome mesh bakes into the stop weights. Raise it if "
							  "you can still see where one colour hands over to the next."));
	}

	ImGui::SliderFloat(T(TKEY("dither_strength"), "Dither"), &settings.DitherStrength, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dither_strength_tooltip"),
							  "Breaks up the stair-stepping left when the sky is written to an 8-bit buffer before "
							  "tonemapping. Raise it if you see rings in large flat areas of sky."));
	}

	ImGui::EndDisabled();
}

#undef I18N_KEY_PREFIX
