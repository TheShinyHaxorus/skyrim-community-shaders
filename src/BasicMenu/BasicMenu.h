#pragma once

#include <imgui.h>
#include <string>
#include <unordered_set>
#include <vector>

struct Feature;

// GFxFunctionHandler that wraps the Flash SystemPage.onCategoryButtonPress.
class CSItemPressHandler : public RE::GFxFunctionHandler
{
public:
	explicit CSItemPressHandler(std::int32_t a_csIndex) :
		csIndex(a_csIndex) {}
	void Call(Params& a_params) override;

private:
	std::int32_t csIndex;
};

// Transparent right-side panel for toggling graphics features.
class BasicMenu : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
	static BasicMenu* GetSingleton();

	void Register();
	void Draw();

	bool IsActive() const { return active; }
	void Activate();
	void Deactivate();

private:
	BasicMenu() = default;

	// Journal menu injection
	bool SetupJournalMenu();
	RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_evn, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

	// Per-feature entry with curated description and perf cost
	struct FeatureEntry
	{
		Feature* feature;
		const char* description;
		const char* badge;   // e.g. "[GPU]", "[VRAM/GPU]"
		int perfLevel;       // 0=light(green), 1=medium(yellow), 2=heavy(red)
	};

	void RebuildFeatureList();
	void DrawListView();
	void DrawFeatureToggle(const FeatureEntry& a_entry);
	void DrawFeatureDetail(const FeatureEntry& a_entry);
	void DrawVRAM();
	void DrawInputHints();

	// Custom toggle switch widget (ported from FUCK, MIT License)
	static bool DrawToggle(const char* a_label, bool* a_value);
	// Custom outlined button widget (ported from FUCK, MIT License)
	// a_width: explicit button width, or 0 for auto-sizing
	static bool DrawButton(const char* a_label, float a_width = 0.0f);

	// Input mode: auto-switches based on last device used
	enum class InputMode
	{
		kMouseKeyboard,
		kGamepad
	};
	InputMode GetInputMode() const;

	// State
	bool active = false;
	bool firstFrame = false;
	InputMode inputMode = InputMode::kMouseKeyboard;
	std::vector<FeatureEntry> featureList;
	std::unordered_set<std::string> userToggledOff;  // features disabled by user this session

	// Detail view
	int selectedFeatureIdx = -1;  // -1 = list view, >= 0 = detail view for that feature
};
