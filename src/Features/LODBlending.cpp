#include "LODBlending.h"

#include "../I18n/I18n.h"
#include "State.h"

#define I18N_KEY_PREFIX "feature.lod_blending."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LODBlending::Settings,
	LODTerrainBrightness,
	LODObjectBrightness,
	LODObjectSnowBrightness,
	DisableTerrainVertexColors,
	LODTerrainGamma,
	LODObjectGamma,
	LODObjectSnowGamma,
	EnableTreeWind,
	TreeWindAmplitude,
	TreeWindSpeed)

void LODBlending::DrawSettings()
{
	ImGui::SliderFloat(T(TKEY("lod_terrain_brightness"), "LOD Terrain Brightness"), &settings.LODTerrainBrightness, 0.01f, 5.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("lod_object_brightness"), "LOD Object Brightness"), &settings.LODObjectBrightness, 0.01f, 5.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("lod_object_snow_brightness"), "LOD Object Snow Brightness"), &settings.LODObjectSnowBrightness, 0.01f, 5.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("lod_terrain_gamma"), "LOD Terrain Gamma"), &settings.LODTerrainGamma, 0.1f, 3.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("lod_object_gamma"), "LOD Object Gamma"), &settings.LODObjectGamma, 0.1f, 3.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("lod_object_snow_gamma"), "LOD Object Snow Gamma"), &settings.LODObjectSnowGamma, 0.1f, 3.f, "%.2f");
	ImGui::Checkbox(T(TKEY("disable_terrain_vertex_colors"), "Disable Terrain Vertex Colors"), (bool*)&settings.DisableTerrainVertexColors);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("disable_terrain_vertex_colors_tooltip"),
							  "Disables vertex coloring on nearby terrain. Best combined with terrain LOD generated in xLODGen with Vertex Color Intensity set to 0."));
	}

	ImGui::Spacing();
	ImGui::SeparatorText(T(TKEY("tree_wind"), "Distant Tree Wind"));

	ImGui::Checkbox(T(TKEY("enable_tree_wind"), "Enable Distant Tree Wind"), (bool*)&settings.EnableTreeWind);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tree_wind_tooltip"),
							  "Sways distant tree LOD billboards, which the game leaves completely static. Without this the treeline freezes at the distance where full trees are swapped for billboards."));
	}

	ImGui::BeginDisabled(!settings.EnableTreeWind);
	ImGui::SliderFloat(T(TKEY("tree_wind_amplitude"), "Tree Wind Amplitude"), &settings.TreeWindAmplitude, 0.f, 0.15f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("tree_wind_amplitude_tooltip"),
							  "Sway distance at the top of a billboard, as a fraction of its height. The base never moves."));
	}
	ImGui::SliderFloat(T(TKEY("tree_wind_speed"), "Tree Wind Speed"), &settings.TreeWindSpeed, 0.1f, 4.f, "%.2f");
	ImGui::EndDisabled();
}

#undef I18N_KEY_PREFIX

void LODBlending::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LODBlending::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LODBlending::RestoreDefaultSettings()
{
	settings = {};
}

void LODBlending::PostPostLoad()
{
	Hooks::Install();
}

LODBlending::BufferData LODBlending::GetCommonBufferData()
{
	auto* state = globals::state;

	// UpdateSharedData can run more than once per frame; only step the history on a new frame
	// so the previous timer stays a true one-frame delta for the motion vectors.
	if (state->frameCount != lastWindFrame) {
		previousWindTimer = windTimer;
		windTimer = state->timer;
		lastWindFrame = state->frameCount;
	}

	BufferData data{};
	data.settings = settings;
	data.TreeWindTimer = windTimer;
	data.TreeWindPreviousTimer = previousWindTimer;
	return data;
}

void LODBlending::Update()
{
	// Bind whenever loaded, not just when the wind is enabled: the vertex shader reads the
	// enable flag out of b6, so leaving a foreign buffer bound there would let stale data
	// switch the sway back on with garbage parameters.
	if (!loaded)
		return;

	auto* state = globals::state;
	ID3D11Buffer* buffers[] = { state->sharedDataCB->CB(), state->featureDataCB->CB() };
	globals::d3d::context->VSSetConstantBuffers(5, ARRAYSIZE(buffers), buffers);
}

void LODBlending::Hooks::BSDistantTreeShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::lodBlending.Update();
	func(This, Pass, RenderFlags);
}
