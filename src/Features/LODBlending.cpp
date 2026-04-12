#include "LODBlending.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LODBlending::Settings,
	LODTerrainBrightness,
	LODObjectBrightness,
	LODObjectSnowBrightness,
	DisableTerrainVertexColors,
	LODTerrainGamma,
	LODObjectGamma,
	LODObjectSnowGamma,
	LODTreeBrightness,
	LODTreeGamma)

void LODBlending::DrawSettings()
{
	if (ImGui::TreeNodeEx("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Brightness##Terrain", &settings.LODTerrainBrightness, 0.01f, 5.f, "%.2f");
		ImGui::SliderFloat("Gamma##Terrain", &settings.LODTerrainGamma, 0.1f, 3.f, "%.2f");
		ImGui::Checkbox("Disable Vertex Colors", (bool*)&settings.DisableTerrainVertexColors);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Disables vertex coloring on nearby terrain. "
				"Best combined with terrain LOD generated in xLODGen with Vertex Color Intensity set to 0.");
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Trees (Billboards & Object LOD)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Brightness##Tree", &settings.LODTreeBrightness, 0.01f, 5.f, "%.2f");
		ImGui::SliderFloat("Gamma##Tree", &settings.LODTreeGamma, 0.1f, 3.f, "%.2f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Other LOD Objects", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Brightness##Object", &settings.LODObjectBrightness, 0.01f, 5.f, "%.2f");
		ImGui::SliderFloat("Gamma##Object", &settings.LODObjectGamma, 0.1f, 3.f, "%.2f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Snow on LOD Objects", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Brightness##Snow", &settings.LODObjectSnowBrightness, 0.01f, 5.f, "%.2f");
		ImGui::SliderFloat("Gamma##Snow", &settings.LODObjectSnowGamma, 0.1f, 3.f, "%.2f");
		ImGui::TreePop();
	}
}

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