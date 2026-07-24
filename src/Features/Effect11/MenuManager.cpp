#include "MenuManager.h"

#include "EffectManager.h"
#include "SettingManager.h"
#include "TextureManager.h"
#include "Utils/ShaderPatches.h"

#include "Features/Effect11.h"
#include "Globals.h"

#include <imgui_stdlib.h>

static const char* const timeOfDayNames[] = { "Dawn", "Sunrise", "Day", "Sunset", "Dusk", "Night", "InteriorDay", "InteriorNight" };

namespace
{
	bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
	{
		if (needle.empty())
			return true;
		auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
			[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
		return it != haystack.end();
	}
}

MenuManager& MenuManager::GetSingleton()
{
	static MenuManager instance;
	return instance;
}

void MenuManager::RenderImGui()
{
	if (!ImGui::BeginTable("Effect11", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV, ImVec2(0, 0))) {
		return;
	}

	ImGui::TableSetupColumn("Settings", ImGuiTableColumnFlags_WidthFixed, 400.0f);
	ImGui::TableSetupColumn("Effects", ImGuiTableColumnFlags_WidthStretch);

	ImGui::TableNextRow();

	// Left side - Settings
	ImGui::TableSetColumnIndex(0);
	if (ImGui::BeginChild("Settings", ImVec2(0, 0), false)) {
		RenderSettingsPanel();
	}
	ImGui::EndChild();

	// Right side - Effects
	ImGui::TableSetColumnIndex(1);
	if (ImGui::BeginChild("Effects", ImVec2(0, 0), false)) {
		EffectManager::GetSingleton().RenderEffectsList();
	}
	ImGui::EndChild();

	ImGui::EndTable();
}

void MenuManager::RenderSettingsPanel()
{
	auto& settingManager = SettingManager::GetSingleton();
	auto& effectManager = EffectManager::GetSingleton();

	if (ImGui::Button("Save & Apply")) {
		settingManager.Save();
		effectManager.Save();
		Util::ShaderPatches::Load();
		settingManager.Load();
		effectManager.Apply();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Save all settings, then reload and recompile shaders");
	}

	ImGui::SameLine();

	if (ImGui::Button("Load & Apply")) {
		Util::ShaderPatches::Load();
		settingManager.Load();
		effectManager.Apply();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Load all settings from enbseries.ini, weather files, and effect configurations, reload shaders");
	}

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	if (ImGui::Button("Save")) {
		settingManager.Save();
		effectManager.Save();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Save all settings to enbseries.ini, weather files, and effect configurations (no shader reload)");
	}

	ImGui::SameLine();

	if (ImGui::Button("Load")) {
		settingManager.Load();
		effectManager.Load();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Load all settings from enbseries.ini, weather files, and effect configurations (no shader reload)");
	}

	if (settingManager.HasUnsavedChanges() || effectManager.AnyUnsaved()) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f), "unsaved changes");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Settings or effect parameters differ from what is on disk. Click Save to persist them.");
		}
	}

	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##Effect11SettingsFilter", "Search settings...", &settingsFilter);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Filter settings by name or category across all tabs");
	}

	ImGui::Separator();

	if (ImGui::BeginChild("SettingsScroll", ImVec2(0, 0), false)) {
		RenderAllSettings();
	}
	ImGui::EndChild();
}

void MenuManager::RenderWeatherControl()
{
	auto& effectManager = EffectManager::GetSingleton();

	// Current weather status
	uint32_t currentWeatherID = static_cast<uint32_t>(effectManager.commonData.weather[0]);
	uint32_t lastWeatherID = static_cast<uint32_t>(effectManager.commonData.weather[1]);
	float blendFactor = effectManager.commonData.weather[2];

	ImGui::Text("Current Weather: 0x%X, Outgoing Weather: 0x%X", currentWeatherID, lastWeatherID);
	ImGui::Text("Weather Blend Factor: %.2f", blendFactor);

	uint32_t editTargetID = blendFactor > 0.5f ? currentWeatherID : lastWeatherID;
	auto* entry = WeatherManager::GetSingleton().FindWeatherEntry(editTargetID);
	if (entry) {
		ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "Edits target: %s (0x%X)", entry->fileName.c_str(), editTargetID);
	} else {
		ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "Edits target: enbseries.ini (no weather override active)");
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Where edits to weather-supported settings are stored right now.\n"
			"When a listed weather is dominant (blend), edits go to its weather ini;\n"
			"otherwise they go to the global enbseries.ini values.");
	}
}

void MenuManager::RenderDebugControl()
{
	auto& effectManager = EffectManager::GetSingleton();
	auto& weatherManager = WeatherManager::GetSingleton();

	if (ImGui::TreeNodeEx("Vanilla Tonemap Constants (Params01)", ImGuiTreeNodeFlags_DefaultOpen)) {
		auto& effect11 = globals::features::effect11;

		if (!effect11.vanillaTonemapParamsValid) {
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Vanilla tonemap not rendered this frame (Effect11 active) - enbeffect.fx uses reconstructed values");
		}

		static const char* const rowNames[7] = {
			"[0] Flags", "[1] TimingData", "[2] Param (bloom/white)",
			"[3] Cinematic (sat/-/con/bri)", "[4] Tint", "[5] Fade", "[6] BlurScale"
		};

		if (ImGui::BeginTable("Params01Debug", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Constant", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			for (int i = 0; i < 7; ++i) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", rowNames[i]);
				ImGui::TableSetColumnIndex(1);
				const auto& v = effect11.vanillaTonemapParams[i];
				ImGui::Text("%.3f  %.3f  %.3f  %.3f", v.x, v.y, v.z, v.w);
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}

	ImGui::Separator();

	// Current time of day values
	if (ImGui::BeginTable("TimeOfDayValues", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
		ImGui::TableSetupColumn("Period", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();

		const char* tod1Names[] = { "Dawn", "Sunrise", "Day", "Sunset" };
		for (int i = 0; i < 4; ++i) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", tod1Names[i]);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", effectManager.commonData.timeOfDay1[i]);
		}

		const char* tod2Names[] = { "Dusk", "Night", "InteriorDay", "InteriorNight" };
		for (int i = 0; i < 4; ++i) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", tod2Names[i]);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", effectManager.commonData.timeOfDay2[i]);
		}

		ImGui::EndTable();
	}

	ImGui::Separator();

	// Weather file list
	if (ImGui::TreeNodeEx("Loaded Weather Files", ImGuiTreeNodeFlags_DefaultOpen)) {
		const auto& weatherEntries = weatherManager.GetWeatherEntries();

		if (!weatherEntries.empty()) {
			if (ImGui::BeginChild("WeatherList", ImVec2(0, 300), true)) {
				// Sort weather entries by name for consistent display
				std::vector<std::pair<std::string, const WeatherManager::WeatherEntry*>> sortedWeathers;
				for (const auto& [key, entry] : weatherEntries) {
					sortedWeathers.emplace_back(key, &entry);
				}
				std::sort(sortedWeathers.begin(), sortedWeathers.end());

				for (const auto& [key, entry] : sortedWeathers) {
					ImGui::PushID(key.c_str());

					// Show weather file name and IDs
					ImGui::Text("%s", entry->fileName.c_str());
					ImGui::SameLine();
					ImGui::Text("(%s)", key.c_str());

					// Show weather IDs on same line
					ImGui::SameLine();
					std::string idsText = "IDs: ";
					for (size_t i = 0; i < entry->weatherIDs.size() && i < 3; ++i) {
						if (i > 0)
							idsText += ", ";
						idsText += std::format("0x{:X}", entry->weatherIDs[i]);
					}
					if (entry->weatherIDs.size() > 3) {
						idsText += "...";
					}
					ImGui::Text("%s", idsText.c_str());

					ImGui::PopID();
				}
			}
			ImGui::EndChild();
		} else {
			ImGui::Text("No weather files loaded");
			ImGui::Text("Make sure _weatherlist.ini exists in enbseries folder");
		}

		ImGui::TreePop();
	}
}

std::vector<int> MenuManager::GetActiveTimeOfDayIndices() const
{
	auto& effectManager = EffectManager::GetSingleton();
	std::vector<int> activeIndices;

	// Access time of day data from EffectManager (this data is updated every frame)
	const auto& commonData = effectManager.commonData;

	// Check if we're in interior (> 0.5) or exterior
	bool isInterior = commonData.eInteriorFactor > 0.5f;

	if (isInterior) {
		// For interiors, show both interior time periods
		activeIndices.push_back(6);  // InteriorDay
		activeIndices.push_back(7);  // InteriorNight
	} else {
		// For exteriors, show all exterior time periods
		activeIndices.push_back(0);  // Dawn
		activeIndices.push_back(1);  // Sunrise
		activeIndices.push_back(2);  // Day
		activeIndices.push_back(3);  // Sunset
		activeIndices.push_back(4);  // Dusk
		activeIndices.push_back(5);  // Night
	}

	return activeIndices;
}

float MenuManager::GetTimeOfDayBlendFactor(int timeIndex) const
{
	auto& effectManager = EffectManager::GetSingleton();
	const auto& commonData = effectManager.commonData;

	// Return the actual blend factor for each time period
	switch (timeIndex) {
	case 0:
		return commonData.timeOfDay1[0];  // Dawn
	case 1:
		return commonData.timeOfDay1[1];  // Sunrise
	case 2:
		return commonData.timeOfDay1[2];  // Day
	case 3:
		return commonData.timeOfDay1[3];  // Sunset
	case 4:
		return commonData.timeOfDay2[0];  // Dusk
	case 5:
		return commonData.timeOfDay2[1];  // Night
	case 6:
		return commonData.timeOfDay2[2];  // InteriorDay
	case 7:
		return commonData.timeOfDay2[3];  // InteriorNight
	default:
		return 0.0f;
	}
}

void MenuManager::RenderAllSettings()
{
	auto& settingManager = SettingManager::GetSingleton();
	auto categorizedSettings = settingManager.GetCategorizedSettings();

	const bool filtering = !settingsFilter.empty();

	// Define explicit order for tabs
	const std::vector<std::string> tabOrder = { "Main", "Weather", "Debug" };

	if (ImGui::BeginTabBar("SettingsTabBar", ImGuiTabBarFlags_None)) {
		for (const auto& tabName : tabOrder) {
			if (categorizedSettings.find(tabName) == categorizedSettings.end())
				continue;

			const auto& categories = categorizedSettings[tabName];

			if (ImGui::BeginTabItem(tabName.c_str())) {
				// Add weather control to the Weather tab
				if (tabName == "Weather") {
					RenderWeatherControl();
					ImGui::Separator();
				}

				if (tabName == "Debug") {
					RenderDebugControl();
				}

				for (const auto& category : categories) {
					if (!settingManager.IsCategoryEnabled(category))
						continue;

					auto settings = settingManager.GetSettingsByCategory(category);

					std::erase_if(settings, [&](const std::string& key) {
						auto settingInfo = settingManager.GetSettingInfo(key, category);
						if (!settingInfo)
							return true;

						const bool isTod = settingInfo->type == SettingType::TimeOfDay || settingInfo->type == SettingType::ColorTimeOfDay;
						if ((tabName == "Main" && isTod) || (tabName == "Weather" && !isTod))
							return true;

						return !settingManager.IsSettingEnabled(key, category);
					});

					// With a search active, show only matching settings; a match
					// on the category name keeps the whole category visible.
					if (filtering && !ContainsCaseInsensitive(category, settingsFilter)) {
						std::erase_if(settings, [&](const std::string& key) {
							return !ContainsCaseInsensitive(key, settingsFilter);
						});
					}

					if (settings.empty())
						continue;

					ImGuiTreeNodeFlags flags = (tabName == "Weather") ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen;
					if (filtering)
						ImGui::SetNextItemOpen(true);

					if (ImGui::TreeNodeEx(category.c_str(), flags)) {
						if (ImGui::BeginTable((category + "_table").c_str(), 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
							ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed);
							ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

							// Add weather ignore controls for categories with weather support (Weather tab only)
							if (tabName == "Weather" && settingManager.CategoryHasWeatherSupport(category)) {
								auto& effectManager = EffectManager::GetSingleton();
								bool isInterior = effectManager.commonData.eInteriorFactor > 0.5f;

								if (!isInterior) {
									// Show exterior ignore setting when outside
									ImGui::TableNextRow();
									ImGui::TableSetColumnIndex(0);
									ImGui::Text("IgnoreWeatherSystem");
									ImGui::TableSetColumnIndex(1);
									bool ignoreWeather = settingManager.GetIgnoreWeatherSystem(category);
									if (ImGui::Checkbox(("##IgnoreWeatherSystem_" + category).c_str(), &ignoreWeather)) {
										settingManager.SetIgnoreWeatherSystem(category, ignoreWeather);
									}
									if (ImGui::IsItemHovered()) {
										ImGui::SetTooltip("When enabled, uses enbseries.ini values instead of weather-specific values for exterior areas");
									}
								} else if (!settingManager.IsCategoryExteriorOnly(category)) {
									// Show interior ignore setting when inside (skip for exterior-only categories)
									ImGui::TableNextRow();
									ImGui::TableSetColumnIndex(0);
									ImGui::Text("IgnoreWeatherSystemInterior");
									ImGui::TableSetColumnIndex(1);
									bool ignoreWeatherInterior = settingManager.GetIgnoreWeatherSystemInterior(category);
									if (ImGui::Checkbox(("##IgnoreWeatherSystemInterior_" + category).c_str(), &ignoreWeatherInterior)) {
										settingManager.SetIgnoreWeatherSystemInterior(category, ignoreWeatherInterior);
									}
									if (ImGui::IsItemHovered()) {
										ImGui::SetTooltip("When enabled, uses enbseries.ini values instead of weather-specific values for interior areas");
									}
								}

								// Add separator
								ImGui::TableNextRow();
								ImGui::TableSetColumnIndex(0);
								ImGui::Separator();
								ImGui::TableSetColumnIndex(1);
								ImGui::Separator();
							}

							for (const auto& settingKey : settings) {
								RenderSettingRows(settingKey, category);
							}

							ImGui::EndTable();
						}
						ImGui::TreePop();
					}
				}
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}
}

void MenuManager::RenderSettingRows(const std::string& settingKey, const std::string& category)
{
	auto& settingManager = SettingManager::GetSingleton();
	auto settingInfo = settingManager.GetSettingInfo(settingKey, category);
	if (!settingInfo)
		return;

	uint32_t settingID = settingInfo->id;

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);

	switch (settingInfo->type) {
	case SettingType::Bool:
		{
			ImGui::Text("%s", settingKey.c_str());
			ImGui::TableSetColumnIndex(1);
			bool v = settingManager.GetValue<bool>(settingID, true);
			if (ImGui::Checkbox(("##" + settingKey).c_str(), &v)) {
				settingManager.SetValue<bool>(settingID, v);
			}
			break;
		}
	case SettingType::Float:
		{
			ImGui::Text("%s", settingKey.c_str());
			ImGui::TableSetColumnIndex(1);
			float v = settingManager.GetValue<float>(settingID, true);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragFloat(("##" + settingKey).c_str(), &v, settingInfo->step, settingInfo->minValue, settingInfo->maxValue, "%.2f")) {
				v = std::clamp(v, settingInfo->minValue, settingInfo->maxValue);
				settingManager.SetValue<float>(settingID, v);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Drag to adjust, Ctrl+Click to type\nRange: %.2f to %.2f", settingInfo->minValue, settingInfo->maxValue);
			}
			break;
		}
	case SettingType::TimeOfDay:
		{
			// Collapsed by default: one row showing the blended value currently
			// in effect; expand to edit the individual time-of-day values.
			bool open = ImGui::TreeNodeEx(settingKey.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("now: %.2f", settingManager.GetInterpolatedTimeOfDayValue(settingKey, category));
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Blended value currently in effect. Expand the row to edit per time of day.");
			}

			if (open) {
				auto v = settingManager.GetValue<TimeOfDayValue>(settingID, true);
				bool exteriorOnly = settingManager.IsCategoryExteriorOnly(category);
				bool changed = false;

				for (int i = 0; i < 8; ++i) {
					if (exteriorOnly && i >= 6)
						continue;

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);

					// Dim time periods that are not currently contributing
					float blendFactor = GetTimeOfDayBlendFactor(i);
					bool isActive = blendFactor > 0.0f;
					if (!isActive)
						ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);

					if (isActive)
						ImGui::Text("%s (%.0f%%)", timeOfDayNames[i], blendFactor * 100.0f);
					else
						ImGui::Text("%s", timeOfDayNames[i]);

					ImGui::TableSetColumnIndex(1);
					ImGui::SetNextItemWidth(-FLT_MIN);
					std::string label = "##" + settingKey + std::to_string(i);
					if (ImGui::DragFloat(label.c_str(), &v.values[i], settingInfo->step, settingInfo->minValue, settingInfo->maxValue, "%.2f")) {
						v.values[i] = std::clamp(v.values[i], settingInfo->minValue, settingInfo->maxValue);
						changed = true;
					}

					if (!isActive)
						ImGui::PopStyleVar();
				}

				if (changed) {
					settingManager.SetValue<TimeOfDayValue>(settingID, v);
				}

				ImGui::TreePop();
			}
			break;
		}
	case SettingType::ColorTimeOfDay:
		{
			bool open = ImGui::TreeNodeEx(settingKey.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
			ImGui::TableSetColumnIndex(1);
			float3 now = settingManager.GetInterpolatedColorTimeOfDayValue(settingKey, category);
			ImGui::ColorButton(("##now_" + settingKey).c_str(), ImVec4(now.x, now.y, now.z, 1.0f), ImGuiColorEditFlags_NoTooltip);
			ImGui::SameLine();
			ImGui::TextDisabled("now");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Blended color currently in effect. Expand the row to edit per time of day.");
			}

			if (open) {
				auto v = settingManager.GetValue<ColorTimeOfDayValue>(settingID, true);
				bool exteriorOnly = settingManager.IsCategoryExteriorOnly(category);
				bool changed = false;

				for (int i = 0; i < 8; ++i) {
					if (exteriorOnly && i >= 6)
						continue;

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);

					float blendFactor = GetTimeOfDayBlendFactor(i);
					bool isActive = blendFactor > 0.0f;
					if (!isActive)
						ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);

					if (isActive)
						ImGui::Text("%s (%.0f%%)", timeOfDayNames[i], blendFactor * 100.0f);
					else
						ImGui::Text("%s", timeOfDayNames[i]);

					ImGui::TableSetColumnIndex(1);
					std::string label = "##" + settingKey + std::to_string(i);
					float color[3] = { v.values[i].x, v.values[i].y, v.values[i].z };

					if (ImGui::ColorEdit3(label.c_str(), color, ImGuiColorEditFlags_NoInputs)) {
						v.values[i].x = color[0];
						v.values[i].y = color[1];
						v.values[i].z = color[2];
						changed = true;
					}

					if (!isActive)
						ImGui::PopStyleVar();
				}

				if (changed) {
					settingManager.SetValue<ColorTimeOfDayValue>(settingID, v);
				}

				ImGui::TreePop();
			}
			break;
		}
	}
}
