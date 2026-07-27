#pragma once

#include "EffectManager.h"
#include "WeatherManager.h"

class MenuManager
{
public:
	static MenuManager& GetSingleton();

	// Main UI rendering method
	void RenderImGui();

private:
	// UI section rendering methods
	void RenderToolbar();
	void RenderWeatherControl();
	void RenderDebugControl();

	// Helper UI methods
	void RenderTabs();
	void RenderSettingsFilter();
	void RenderSettingRows(const std::string& settingKey, const std::string& category);
	std::vector<int> GetActiveTimeOfDayIndices() const;
	float GetTimeOfDayBlendFactor(int timeIndex) const;

	// Case-insensitive search filter applied to setting names and categories
	std::string settingsFilter;
};
