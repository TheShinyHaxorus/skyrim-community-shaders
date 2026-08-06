#include "LODBlending.h"

#include "../I18n/I18n.h"
#include "State.h"
#include "TerrainShadows.h"

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
	TreeWindSpeed,
	EnableLODObjectTreeWind,
	LODObjectTreeWindAmplitude,
	LODObjectTreeHeight)

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

	ImGui::Spacing();

	ImGui::Checkbox(T(TKEY("enable_lod_object_tree_wind"), "Sway 3D Tree LOD"), (bool*)&settings.EnableLODObjectTreeWind);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_lod_object_tree_wind_tooltip"),
							  "DynDOLOD 3D (Ultra) tree LOD renders trees as merged object LOD instead of billboards, so the "
							  "billboard sway above never reaches them. This sways those merged shapes instead.\n\n"
							  "Requires Terrain Shadows: its worldspace heightmap is what tells the shader how far each "
							  "vertex sits above the ground, which is the only way to keep trunks planted while canopies move."));
	}

	const bool heightmapReady = globals::features::terrainShadows.loaded &&
	                            globals::features::terrainShadows.IsHeightMapReady();
	if (settings.EnableLODObjectTreeWind && !heightmapReady) {
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "%s",
			T(TKEY("lod_object_tree_wind_no_heightmap"),
				"Inactive: no Terrain Shadows heightmap for this worldspace."));
	}

	ImGui::BeginDisabled(!settings.EnableLODObjectTreeWind);
	ImGui::SliderFloat(T(TKEY("lod_object_tree_wind_amplitude"), "3D Tree LOD Sway Distance"), &settings.LODObjectTreeWindAmplitude, 0.f, 80.f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("lod_object_tree_wind_amplitude_tooltip"),
							  "Sway distance in game units at full canopy height. The trunk base stays planted."));
	}
	ImGui::SliderFloat(T(TKEY("lod_object_tree_height"), "3D Tree LOD Height"), &settings.LODObjectTreeHeight, 300.f, 4000.f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("lod_object_tree_height_tooltip"),
							  "Height above the ground at which the sway reaches full strength, in game units. "
							  "Roughly the height of your LOD trees. Too low and trunks move; too high and the canopy stays stiff."));
	}
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

void LODBlending::StepWindTimer()
{
	auto* state = globals::state;

	// UpdateSharedData can run more than once per frame; only step the history on a new frame
	// so the previous timer stays a true one-frame delta for the motion vectors.
	if (state->frameCount != lastWindFrame) {
		previousWindTimer = windTimer;
		windTimer = state->timer;
		lastWindFrame = state->frameCount;
	}
}

LODBlending::BufferData LODBlending::GetCommonBufferData()
{
	StepWindTimer();

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

void LODBlending::SetupLODTreeWindResources()
{
	if (!lodTreeWindCB)
		lodTreeWindCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<LODTreeWindData>(), "LODBlending::LODTreeWindCB");
}

// LODGen names merged object-LOD shapes after their material class, and DynDOLOD's 3D tree LOD
// lands in the "passthru" groups. Nothing else about the draw survives the merge, so the name is
// the only discriminator available here.
static bool IsLODTreeShape(const RE::BSGeometry* a_geometry)
{
	if (!a_geometry)
		return false;

	const char* name = a_geometry->name.c_str();
	if (!name)
		return false;

	const std::string_view view{ name };
	return view.starts_with("treepassthru") || view.starts_with("objpassthru");
}

void LODBlending::UpdateLODObjectTreeWind(RE::BSRenderPass* a_pass)
{
	if (!loaded)
		return;

	SetupLODTreeWindResources();
	if (!lodTreeWindCB)
		return;

	auto& terrainShadows = globals::features::terrainShadows;
	const bool heightmapReady = terrainShadows.loaded && terrainShadows.texHeightMap &&
	                            terrainShadows.IsHeightMapReady();

	const bool isTree = settings.EnableLODObjectTreeWind && heightmapReady &&
	                    a_pass && IsLODTreeShape(a_pass->geometry);

	// Draws are sorted by shader and material, so the buffer is only rewritten when a pass crosses
	// between tree LOD and everything else rather than once per draw call.
	if (isTree != lodTreeWindActive || !lodTreeWindEverBound) {
		LODTreeWindData data{};

		if (isTree) {
			StepWindTimer();

			const auto terrain = terrainShadows.GetCommonBufferData();

			data.Enabled = 1.f;
			data.InvTreeHeight = 1.f / std::max(settings.LODObjectTreeHeight, 1.f);
			data.Amplitude = settings.LODObjectTreeWindAmplitude;
			data.Speed = settings.TreeWindSpeed;
			data.Timer = windTimer;
			data.PreviousTimer = previousWindTimer;
			data.MapScale = { terrain.Scale.x, terrain.Scale.y };
			data.MapOffset = terrain.Offset;
			data.MapZRange = terrain.ZRange;
			data.MapDimensions = { static_cast<float>(terrainShadows.texHeightMap->desc.Width),
				static_cast<float>(terrainShadows.texHeightMap->desc.Height) };
		}

		lodTreeWindCB->Update(data);
		lodTreeWindActive = isTree;
		lodTreeWindEverBound = true;
	}

	auto* context = globals::d3d::context;

	ID3D11Buffer* buffers[] = { lodTreeWindCB->CB() };
	context->VSSetConstantBuffers(4, ARRAYSIZE(buffers), buffers);

	if (isTree) {
		ID3D11ShaderResourceView* srvs[] = { terrainShadows.texHeightMap->srv.get() };
		context->VSSetShaderResources(61, ARRAYSIZE(srvs), srvs);
	}
}

void LODBlending::Hooks::BSDistantTreeShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::lodBlending.Update();
	func(This, Pass, RenderFlags);
}

void LODBlending::Hooks::LODBlending_BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::lodBlending.UpdateLODObjectTreeWind(Pass);
	func(This, Pass, RenderFlags);
}
