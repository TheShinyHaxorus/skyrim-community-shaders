#include "BasicMenu.h"

#include "Feature.h"
#include "FeatureCategories.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"
#include "Menu.h"
#include "Utils/UI.h"
#include "State.h"

#include <algorithm>
#include <imgui_internal.h>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

// Windows PlaySound macro conflicts with RE::PlaySound
#ifdef PlaySound
#	undef PlaySound
#endif

namespace
{
	// Helper to call RE::PlaySound without Windows macro interference
	void PlayGameSound(const char* a_editorID)
	{
		RE::PlaySound(a_editorID);
	}
}

// Window flags — no title bar for a cleaner, game-like panel
static constexpr ImGuiWindowFlags kWindowFlags =
	ImGuiWindowFlags_NoCollapse |
	ImGuiWindowFlags_NoSavedSettings |
	ImGuiWindowFlags_NoMove |
	ImGuiWindowFlags_NoResize |
	ImGuiWindowFlags_NoTitleBar;

BasicMenu* BasicMenu::GetSingleton()
{
	static BasicMenu instance;
	return &instance;
}

void BasicMenu::Register()
{
	RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(this);
	logger::info("BasicMenu: registered for MenuOpenCloseEvent");
}

// --- GFx wrapper for SystemPage.onCategoryButtonPress ---
// Flash dispatches by event.index (hardcoded switch).

void CSItemPressHandler::Call(Params& a_params)
{
	if (a_params.argCount < 1)
		return;

	RE::GFxValue& event = a_params.args[0];
	RE::GFxValue indexVal;
	event.GetMember("index", &indexVal);
	if (!indexVal.IsNumber())
		return;

	auto index = static_cast<std::int32_t>(indexVal.GetNumber());

	if (index == csIndex) {
		// entry was clicked — close journal, activate BasicMenu
		auto* queue = RE::UIMessageQueue::GetSingleton();
		queue->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		BasicMenu::GetSingleton()->Activate();
		return;
	}

	// Adjust index for entries that shifted due to insertion
	if (index > csIndex) {
		event.SetMember("index", RE::GFxValue(static_cast<double>(index - 1)));
	}

	// Forward to original Flash handler
	if (a_params.thisPtr) {
		a_params.thisPtr->Invoke("_csOrigItemPress", nullptr, a_params.args, a_params.argCount);
	}
}

// --- Journal menu injection ---

bool BasicMenu::SetupJournalMenu()
{
	const auto menu = RE::UI::GetSingleton()->GetMenu<RE::JournalMenu>(RE::JournalMenu::MENU_NAME);
	if (!menu)
		return false;
	const auto view = menu->GetRuntimeData().systemTab.view;

	RE::GFxValue page;
	if (!view || !view->GetVariable(&page, "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc"))
		return false;

	RE::GFxValue categoryList;
	if (!page.GetMember("CategoryList", &categoryList))
		return false;

	RE::GFxValue entryList;
	if (!categoryList.GetMember("entryList", &entryList))
		return false;

	// Read current entries
	static constexpr auto csLabel = "COMMUNITY SHADERS";
	std::vector<std::string> elements;
	const uint32_t count = entryList.GetArraySize();
	for (uint32_t i = 0; i < count; ++i) {
		RE::GFxValue elem;
		if (!entryList.GetElement(i, &elem))
			continue;
		RE::GFxValue textVal;
		if (!elem.GetMember("text", &textVal))
			continue;
		elements.push_back(textVal.GetString());
	}

	// Already present (shouldn't happen since journal is recreated, but guard)
	if (auto it = std::ranges::find(elements, csLabel); it != elements.end())
		return true;

	// Find insertion point — before the bottom cluster (CONTROLS/HELP/QUIT).
	// Try both "$"-prefixed keys and bare text to cover vanilla/SkyUI/DD.
	ptrdiff_t insertPos = -1;
	for (auto key : { "$CONTROLS", "CONTROLS", "$HELP", "HELP", "$QUIT", "QUIT" }) {
		if (auto it = std::ranges::find(elements, key); it != elements.end()) {
			insertPos = std::distance(elements.begin(), it);
			break;
		}
	}
	if (insertPos < 0)
		insertPos = static_cast<ptrdiff_t>(elements.size());

	elements.insert(elements.begin() + insertPos, csLabel);
	const auto csEntryIndex = static_cast<std::int32_t>(insertPos);

	// Rebuild the GFx entry list
	entryList.ClearElements();
	for (auto& text : elements) {
		RE::GFxValue entry;
		view->CreateObject(&entry);
		entry.SetMember("text", text.c_str());
		entryList.PushBack(entry);
	}
	categoryList.Invoke("InvalidateData");

	// Wrap the Flash item-press handler so we can intercept
	RE::GFxValue origHandler;
	if (page.GetMember("onCategoryButtonPress", &origHandler) && origHandler.IsObject()) {
		page.SetMember("_csOrigItemPress", origHandler);

		RE::GFxValue wrapperFunc;
		view->CreateFunction(&wrapperFunc, new CSItemPressHandler(csEntryIndex));
		page.SetMember("onCategoryButtonPress", wrapperFunc);
	}

	return true;
}

RE::BSEventNotifyControl BasicMenu::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_evn,
	RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (!a_evn)
		return RE::BSEventNotifyControl::kContinue;

	if (a_evn->opening && a_evn->menuName == RE::JournalMenu::MENU_NAME) {
		if (!SetupJournalMenu())
			logger::warn("BasicMenu: failed to inject entry into journal system page");
	}

	return RE::BSEventNotifyControl::kContinue;
}

// --- Activation / Deactivation ---

void BasicMenu::Activate()
{
	RebuildFeatureList();
	firstFrame = true;
	selectedFeatureIdx = -1;
	active = true;
}

void BasicMenu::Deactivate()
{
	if (!active)
		return;

	active = false;
	selectedFeatureIdx = -1;

	// Persist changes
	globals::state->Save();
}

BasicMenu::InputMode BasicMenu::GetInputMode() const
{
	return inputMode;
}

// --- Feature list building ---

// Features excluded from the basic menu — advanced/niche features
// that belong in the full settings menu only.
static const std::unordered_set<std::string> kExcludedFeatures = {
	"InverseSquareLighting",
	"LinearLighting",
	"ImageBasedLighting",
	"ExponentialHeightFog",
	"Skylighting",
	"CloudShadows",
};

// Features excluded by category — utility/debug tools
static bool IsExcludedCategory(std::string_view a_cat)
{
	return a_cat == FeatureCategories::kUtility || a_cat == FeatureCategories::kOther;
}

// Curated one-line descriptions, performance badge, and intensity level
// perfLevel: 0=light(green), 1=medium(yellow), 2=heavy(red)
struct FeatureMeta
{
	const char* description;
	const char* badge;    // "[GPU]", "[VRAM/GPU]", etc.
	int perfLevel;
};
static const std::unordered_map<std::string, FeatureMeta> kFeatureMeta = {
	{ "ScreenSpaceGI", { "Ambient occlusion and indirect lighting", "[VRAM/GPU]", 2 } },
	{ "VolumetricLighting", { "God rays and atmospheric light scattering", "[VRAM/GPU]", 2 } },
	{ "ScreenSpaceShadows", { "Contact shadows for small-scale detail", "[GPU]", 2 } },
	{ "VolumetricShadows", { "Shadows cast through volumetric fog", "[GPU]", 2 } },
	{ "LightLimitFix", { "Removes the game's light count cap", "[GPU]", 1 } },
	{ "TerrainShadows", { "Self-shadowing for terrain geometry", "[GPU]", 1 } },
	{ "DynamicCubemaps", { "Real-time reflections on surfaces", "[VRAM/GPU]", 1 } },
	{ "WetnessEffects", { "Rain wetness, puddles, and raindrop effects", "[GPU]", 1 } },
	{ "SubsurfaceScattering", { "Light transmission through skin and foliage", "[GPU]", 1 } },
	{ "TerrainBlending", { "Smooth transitions between terrain and objects", "[GPU]", 0 } },
	{ "TerrainVariation", { "Adds color variation to terrain textures", "[GPU]", 0 } },
	{ "WaterEffects", { "Enhanced water rendering", "[GPU]", 0 } },
	{ "UnifiedWater", { "Improved water shader with blending", "[GPU]", 0 } },
	{ "ExtendedMaterials", { "Parallax, terrain blending, and material enhancements", "[GPU]", 0 } },
	{ "HairSpecular", { "Anisotropic hair highlights", "[GPU]", 0 } },
	{ "GrassLighting", { "Improved grass shading and shadows", "[GPU]", 0 } },
	{ "GrassCollision", { "Grass bends when characters walk through it", "[GPU]", 0 } },
	{ "LODBlending", { "Smooth transitions between LOD levels", "[GPU]", 0 } },
	{ "SkySync", { "Synchronizes sky colors with lighting", "[GPU]", 0 } },
	{ "InteriorSun", { "Directional sun light in interiors", "[GPU]", 0 } },
	{ "ExtendedTranslucency", { "Enhanced translucent material rendering", "[GPU]", 0 } },
	{ "Upscaling", { "FidelityFX/Streamline upscaling", "[VRAM/GPU]", 1 } },
	{ "HDRDisplay", { "HDR output for supported displays", "[GPU]", 0 } },
	{ "TerrainHelper", { "Terrain shader improvements and fixes", "[GPU]", 0 } },
	{ "CloudRelight", { "Dynamic cloud lighting from sun and moon", "[GPU]", 0 } },
};

void BasicMenu::RebuildFeatureList()
{
	featureList.clear();

	for (auto* feat : Feature::GetFeatureList()) {
		if (!feat->IsInMenu())
			continue;

		const auto shortName = feat->GetShortName();

		// Include if currently loaded OR if user toggled it off this session
		if (!feat->loaded && !userToggledOff.contains(shortName))
			continue;

		if (kExcludedFeatures.contains(shortName))
			continue;

		if (IsExcludedCategory(feat->GetCategory()))
			continue;

		const char* desc = "Graphics enhancement";
		const char* badge = "[GPU]";
		int perf = 0;
		if (auto it = kFeatureMeta.find(shortName); it != kFeatureMeta.end()) {
			desc = it->second.description;
			badge = it->second.badge;
			perf = it->second.perfLevel;
		}

		featureList.push_back({ feat, desc, badge, perf });
	}

	// Sort: heavy features first so users see perf-impacting options at the top
	std::ranges::stable_sort(featureList, [](const FeatureEntry& a, const FeatureEntry& b) {
		return a.perfLevel > b.perfLevel;
	});
}

// --- Drawing ---

// Vanilla Skyrim-inspired color palette (ported from FUCK, MIT License)
static constexpr float kBgAlpha = 0.68f;
static constexpr float kBorderSize = 4.0f;

static constexpr ImVec4 kColorBg{ 0.0f, 0.0f, 0.0f, kBgAlpha };
static constexpr ImVec4 kColorBorder{ 0.25f, 0.25f, 0.25f, 1.0f };
static constexpr ImVec4 kColorText{ 0.733f, 0.741f, 0.749f, 1.0f };
static constexpr ImVec4 kColorTextDim{ 0.733f, 0.741f, 0.749f, 0.30f };
static constexpr ImVec4 kColorFrameBg{ 1.0f, 1.0f, 1.0f, 0.063f };
static constexpr ImVec4 kColorFrameBgHovered{ 1.0f, 1.0f, 1.0f, 0.2f };
static constexpr ImVec4 kColorButton{ 0.0f, 0.0f, 0.0f, 1.0f };
static constexpr ImVec4 kColorButtonHovered{ 1.0f, 1.0f, 1.0f, 0.2f };
static constexpr ImVec4 kColorButtonActive{ 0.60f, 0.60f, 0.60f, 1.0f };
static constexpr ImVec4 kColorNavHighlight{ 1.0f, 1.0f, 1.0f, 1.0f };
static constexpr ImVec4 kColorSeparator{ 0.25f, 0.25f, 0.25f, 1.0f };
static constexpr ImVec4 kColorScrollbarBg{ 0.0f, 0.0f, 0.0f, 0.0f };
static constexpr ImVec4 kColorScrollbarGrab{ 0.31f, 0.31f, 0.31f, 1.0f };

// Toggle switch colors (ported from FUCK)
static constexpr ImVec4 kColorToggleRail{ 0.0f, 0.0f, 0.0f, 1.0f };
static constexpr ImVec4 kColorToggleBorder{ 0.25f, 0.25f, 0.25f, 1.0f };
static constexpr ImVec4 kColorToggleBorderActive{ 0.60f, 0.60f, 0.60f, 1.0f };
static constexpr ImVec4 kColorToggleKnob{ 0.0f, 0.0f, 0.0f, 1.0f };

// Performance badge colors
static constexpr ImVec4 kColorPerfHeavy{ 1.0f, 0.35f, 0.35f, 1.0f };
static constexpr ImVec4 kColorPerfMedium{ 1.0f, 0.8f, 0.3f, 1.0f };
static constexpr ImVec4 kColorPerfLight{ 0.5f, 0.9f, 0.5f, 1.0f };

// VRAM bar colors
static constexpr ImVec4 kColorVRAMOk{ 0.3f, 0.8f, 0.3f, 1.0f };
static constexpr ImVec4 kColorVRAMWarn{ 1.0f, 0.8f, 0.2f, 1.0f };
static constexpr ImVec4 kColorVRAMCrit{ 1.0f, 0.3f, 0.3f, 1.0f };

static constexpr int kStyleColorCount = 15;
static constexpr int kStyleVarCount = 3;

static void PushPanelStyle()
{
	ImGui::PushStyleColor(ImGuiCol_WindowBg, kColorBg);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
	ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, kColorTextDim);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, kColorFrameBg);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kColorFrameBgHovered);
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kColorFrameBgHovered);
	ImGui::PushStyleColor(ImGuiCol_Button, kColorButton);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorButtonHovered);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorButtonActive);
	ImGui::PushStyleColor(ImGuiCol_NavCursor, kColorNavHighlight);
	ImGui::PushStyleColor(ImGuiCol_Separator, kColorSeparator);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, kColorScrollbarBg);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kColorScrollbarGrab);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, kBorderSize);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
}

static void PopPanelStyle()
{
	ImGui::PopStyleVar(kStyleVarCount);
	ImGui::PopStyleColor(kStyleColorCount);
}

void BasicMenu::Draw()
{
	if (!active)
		return;

	auto& io = ImGui::GetIO();

	// Auto-detect input mode from last event.
	// Check mouse/keyboard first, then gamepad — so gamepad wins if both
	// fire in the same frame (common when mouse has residual delta).
	bool sawMouseKB = false;
	bool sawGamepad = false;

	if (ImGui::IsKeyPressed(ImGuiKey_MouseLeft) || ImGui::IsKeyPressed(ImGuiKey_MouseRight) ||
		(io.MouseDelta.x * io.MouseDelta.x + io.MouseDelta.y * io.MouseDelta.y) > 1.0f) {
		sawMouseKB = true;
	}
	for (int key = ImGuiKey_Tab; key <= ImGuiKey_KeypadEqual; ++key) {
		if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key))) {
			sawMouseKB = true;
			break;
		}
	}
	for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickRight; ++key) {
		if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key))) {
			sawGamepad = true;
			break;
		}
	}

	// Gamepad takes priority to avoid cursor popping up from residual mouse delta
	if (sawGamepad)
		inputMode = InputMode::kGamepad;
	else if (sawMouseKB)
		inputMode = InputMode::kMouseKeyboard;

	io.MouseDrawCursor = (inputMode == InputMode::kMouseKeyboard);

	// ESC / gamepad B handling
	if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
		if (selectedFeatureIdx >= 0) {
			// Back to list view
			PlayGameSound("UIMenuCancel");
			selectedFeatureIdx = -1;
			firstFrame = true;  // re-seed focus
			return;
		}
		PlayGameSound("UIMenuCancel");
		Deactivate();
		return;
	}

	PushPanelStyle();

	const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
	const float panelWidth = vpSize.x * 0.32f;
	const float panelHeight = vpSize.y * 0.85f;
	const float margin = vpSize.x * 0.02f;

	ImGui::SetNextWindowPos(ImVec2(vpSize.x - panelWidth - margin, vpSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.0f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight), ImGuiCond_Always);
	ImGui::SetNextWindowFocus();

	if (!ImGui::Begin("##CSBasicMenu", nullptr, kWindowFlags)) {
		ImGui::End();
		PopPanelStyle();
		return;
	}

	// Title
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
	ImGui::Text("COMMUNITY SHADERS");
	ImGui::PopStyleColor();
	ImGui::Separator();
	ImGui::Spacing();

	if (selectedFeatureIdx >= 0 && selectedFeatureIdx < static_cast<int>(featureList.size())) {
		DrawFeatureDetail(featureList[selectedFeatureIdx]);
	} else {
		DrawListView();
	}

	// Bottom bar: VRAM + hints
	ImGui::Separator();
	DrawVRAM();
	DrawInputHints();

	ImGui::End();
	PopPanelStyle();

	if (firstFrame)
		firstFrame = false;
}

void BasicMenu::DrawListView()
{
	// Quality preset bar
	{
		ImGui::Text("Quality Preset");
		ImGui::Spacing();

		// Seed gamepad focus on the first interactive widget
		if (firstFrame)
			ImGui::SetKeyboardFocusHere();

		static constexpr const char* presetLabels[] = { "Low", "Medium", "High" };
		const float presetBtnWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

		for (int i = 0; i < 3; ++i) {
			if (i > 0)
				ImGui::SameLine();
			if (DrawButton(presetLabels[i], presetBtnWidth)) {
				auto& ssgi = globals::features::screenSpaceGI;
				if (ssgi.loaded) {
					switch (i) {
					case 0:
						ssgi.settings.NumSlices = 10;
						ssgi.settings.NumSteps = 12;
						ssgi.settings.ResolutionMode = 2;
						ssgi.settings.EnableBlur = true;
						ssgi.settings.EnableGI = true;
						ssgi.recompileFlag = true;
						break;
					case 1:
						ssgi.settings.NumSlices = 4;
						ssgi.settings.NumSteps = 8;
						ssgi.settings.ResolutionMode = 1;
						ssgi.settings.EnableBlur = true;
						ssgi.settings.EnableGI = true;
						ssgi.recompileFlag = true;
						break;
					case 2:
						ssgi.settings.NumSlices = 4;
						ssgi.settings.NumSteps = 8;
						ssgi.settings.ResolutionMode = 0;
						ssgi.settings.EnableBlur = true;
						ssgi.settings.EnableGI = true;
						ssgi.recompileFlag = true;
						break;
					}
				}
				auto& vl = globals::features::volumetricLighting;
				if (vl.loaded) {
					vl.settings.ExteriorQuality = i;
					vl.settings.InteriorQuality = i;
				}
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
	}

	// Scrollable feature list
	const float bottomBarHeight = ImGui::GetFrameHeightWithSpacing() * 3.5f;
	ImGui::BeginChild("##FeatureList", ImVec2(0, -bottomBarHeight), ImGuiChildFlags_NavFlattened);

	for (int i = 0; i < static_cast<int>(featureList.size()); ++i) {
		ImGui::PushID(i);
		DrawFeatureToggle(featureList[i]);

		// A / Enter on a focused feature → open detail view
		if (ImGui::IsItemFocused() &&
			(ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown) || ImGui::IsKeyPressed(ImGuiKey_Enter))) {
			selectedFeatureIdx = i;
			firstFrame = true;
			PlayGameSound("UIMenuOK");
		}
		ImGui::PopID();
	}

	// LB/RB page scroll (vanilla Skyrim pattern)
	if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) {
		ImGui::SetScrollY(ImMax(0.0f, ImGui::GetScrollY() - ImGui::GetWindowHeight() * 0.8f));
	}
	if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) {
		ImGui::SetScrollY(ImMin(ImGui::GetScrollMaxY(), ImGui::GetScrollY() + ImGui::GetWindowHeight() * 0.8f));
	}

	ImGui::EndChild();
}

void BasicMenu::DrawFeatureToggle(const FeatureEntry& a_entry)
{
	ImGui::PushID(a_entry.feature->GetShortName().c_str());

	// Runtime toggle — directly controls whether the feature is active
	bool enabled = a_entry.feature->loaded;

	if (DrawToggle(a_entry.feature->GetName().c_str(), &enabled)) {
		a_entry.feature->loaded = enabled;
		if (!enabled)
			userToggledOff.insert(a_entry.feature->GetShortName());
		else
			userToggledOff.erase(a_entry.feature->GetShortName());
	}

	// Description line with badge — clickable to open detail view
	{
		ImVec4 badgeColor;
		switch (a_entry.perfLevel) {
		case 2:
			badgeColor = kColorPerfHeavy;
			break;
		case 1:
			badgeColor = kColorPerfMedium;
			break;
		default:
			badgeColor = kColorPerfLight;
			break;
		}

		// Compose description + badge into a single selectable line
		// so SameLine positioning issues are avoided
		float badgeWidth = ImGui::CalcTextSize(a_entry.badge).x;

		ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
		std::string descLine = std::string("  ") + a_entry.description;
		if (ImGui::Selectable(descLine.c_str(), false, ImGuiSelectableFlags_None)) {
			// Find this entry's index and open detail
			for (int i = 0; i < static_cast<int>(featureList.size()); ++i) {
				if (featureList[i].feature == a_entry.feature) {
					selectedFeatureIdx = i;
					firstFrame = true;
					PlayGameSound("UIMenuOK");
					break;
				}
			}
		}
		ImGui::PopStyleColor();

		// Draw badge on the same line as the selectable, right-aligned
		// Use previous line's rect to overlay the badge at the correct Y position
		ImVec2 badgePos = ImVec2(
			ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - badgeWidth,
			ImGui::GetItemRectMin().y);
		ImGui::GetWindowDrawList()->AddText(badgePos, ImGui::GetColorU32(badgeColor), a_entry.badge);
	}

	ImGui::Spacing();
	ImGui::PopID();
}

// =====================================================================
// Custom widgets (ported from FUCK by Fuzzlesz, MIT License)
// Simplified to remove FUCK-specific dependencies (IconFont manager,
// FUCKMan scale, Input manager).
// =====================================================================

bool BasicMenu::DrawToggle(const char* a_label, bool* a_value)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext& g = *GImGui;
	const ImGuiID id = window->GetID(a_label);

	const float frameH = ImGui::GetFrameHeight();
	const float width = frameH * 1.25f;

	// Build label text (strip ## hidden ID parts)
	std::string_view labelView(a_label);
	auto doubleHash = labelView.find("##");
	if (doubleHash != std::string_view::npos)
		labelView = labelView.substr(0, doubleHash);

	const ImVec2 labelSize = ImGui::CalcTextSize(labelView.data(), labelView.data() + labelView.size());

	// Layout: label on left, toggle on right
	const ImVec2 pos = window->DC.CursorPos;
	const float availWidth = ImGui::GetContentRegionAvail().x;
	const ImRect bb(pos, ImVec2(pos.x + availWidth, pos.y + frameH));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id))
		return false;

	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
	if (pressed) {
		*a_value = !*a_value;
		ImGui::MarkItemEdited(id);
		PlayGameSound("UIMenuOK");
	}

	// Smooth animation (ported from FUCK)
	float* t_anim = window->DC.StateStorage->GetFloatRef(id, *a_value ? 1.0f : 0.0f);
	const float target = *a_value ? 1.0f : 0.0f;
	const float animSpeed = 12.0f;
	const float dt = g.IO.DeltaTime;
	if (*t_anim < target)
		*t_anim = ImMin(*t_anim + dt * animSpeed, target);
	else if (*t_anim > target)
		*t_anim = ImMax(*t_anim - dt * animSpeed, target);
	const float t = *t_anim;

	ImDrawList* drawList = window->DrawList;

	// Draw label on left, vertically centered
	const float textY = pos.y + (frameH - labelSize.y) * 0.5f;
	drawList->AddText({ pos.x + 4.0f, textY }, ImGui::GetColorU32(ImGuiCol_Text), labelView.data(), labelView.data() + labelView.size());

	// Draw toggle switch on right side
	const float toggleX = pos.x + availWidth - width - 4.0f;

	// Rail
	const float railH = frameH * 0.25f;
	const float railY = pos.y + (frameH - railH) * 0.5f;
	const ImVec2 railMin = { toggleX + 8.0f, railY };
	const ImVec2 railMax = { toggleX + width - 8.0f, railY + railH };

	drawList->AddRectFilled(railMin, railMax, ImGui::GetColorU32(kColorToggleRail), g.Style.FrameRounding);

	// Rail border
	const ImU32 borderCol = (hovered || ImGui::IsItemFocused())
	                             ? ImGui::GetColorU32(kColorToggleBorderActive)
	                             : ImGui::GetColorU32(kColorToggleBorder);
	drawList->AddRect(railMin, railMax, borderCol, g.Style.FrameRounding, 0, 1.0f);

	// Knob
	const float knobRadius = frameH * 0.32f;
	const float knobStart = railMin.x - (knobRadius * 0.1f);
	const float knobEnd = railMax.x + (knobRadius * 0.1f);
	const float knobX = knobStart + (t * (knobEnd - knobStart));
	const ImVec2 knobCenter = { knobX, pos.y + (frameH * 0.5f) };

	drawList->AddCircleFilled(knobCenter, knobRadius, ImGui::GetColorU32(kColorToggleKnob));

	// Knob ring
	const float ringThick = 2.0f;
	const float ringRadius = knobRadius - 4.0f;
	const ImU32 knobRingCol = *a_value
	                              ? ImGui::GetColorU32(ImGuiCol_Text)
	                              : borderCol;
	if (ringRadius > 0.5f)
		drawList->AddCircle(knobCenter, ringRadius, knobRingCol, 0, ringThick);

	// Nav highlight frame for keyboard/gamepad navigation
	if (ImGui::IsItemFocused()) {
		drawList->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_NavCursor), 0.0f, 0, 2.0f);
	}

	return pressed;
}

bool BasicMenu::DrawButton(const char* a_label, float a_width)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext& g = *GImGui;
	const ImGuiID id = window->GetID(a_label);

	const ImVec2 textSize = ImGui::CalcTextSize(a_label);
	const ImVec2 textPad = ImVec2(5.0f, 4.0f);
	const float borderSize = g.Style.FrameBorderSize > 0.0f ? g.Style.FrameBorderSize : kBorderSize;
	const float rounding = 6.0f;

	// Button outline needs extra space for the border
	const float outlining = borderSize + 2.5f;
	const float outlineInset = outlining * 0.5f;

	const ImVec2 contentSize = ImVec2(textSize.x, ImGui::GetTextLineHeight());
	ImVec2 sz = ImVec2(
		contentSize.x + textPad.x * 2.0f + outlineInset * 2.0f,
		contentSize.y + textPad.y * 2.0f + outlineInset * 2.0f);
	sz.x = ImMax(sz.x, sz.y);

	// Use explicit width if provided, otherwise auto-size with minimum
	if (a_width > 0.0f)
		sz.x = a_width;
	else
		sz.x = ImMax(sz.x, sz.y * 2.5f);

	const ImVec2 curPos = window->DC.CursorPos;
	const ImRect bb(curPos, ImVec2(curPos.x + sz.x, curPos.y + sz.y));
	ImGui::ItemSize(sz);
	if (!ImGui::ItemAdd(bb, id))
		return false;

	const ImRect drawBb = {
		ImVec2(bb.Min.x + outlineInset, bb.Min.y + outlineInset),
		ImVec2(bb.Max.x - outlineInset, bb.Max.y - outlineInset)
	};

	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

	// Background fill
	ImGui::RenderFrame(drawBb.Min, drawBb.Max, ImGui::GetColorU32(ImGuiCol_Button), true, rounding);

	// Black outline
	window->DrawList->AddRect(drawBb.Min, drawBb.Max, IM_COL32(0, 0, 0, 255), rounding, 0, outlining);

	// Themed border
	const ImU32 borderCol = (hovered || held || ImGui::IsItemFocused())
	                             ? ImGui::GetColorU32(kColorToggleBorderActive)
	                             : ImGui::GetColorU32(kColorToggleBorder);
	window->DrawList->AddRect(drawBb.Min, drawBb.Max, borderCol, rounding, 0, borderSize);

	// Centered text
	ImGui::RenderTextClipped(bb.Min, bb.Max, a_label, nullptr, &contentSize, { 0.5f, 0.5f });

	// Nav highlight frame for keyboard/gamepad navigation
	if (ImGui::IsItemFocused()) {
		window->DrawList->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_NavCursor), rounding, 0, 2.0f);
	}

	if (pressed)
		PlayGameSound("UIMenuOK");

	return pressed;
}

void BasicMenu::DrawFeatureDetail(const FeatureEntry& a_entry)
{
	// Header: feature name + toggle
	bool enabled = a_entry.feature->loaded;

	// Back button
	if (firstFrame)
		ImGui::SetKeyboardFocusHere();

	if (DrawButton("Back")) {
		selectedFeatureIdx = -1;
		firstFrame = true;
		PlayGameSound("UIMenuCancel");
		return;
	}

	ImGui::Spacing();

	// Feature name
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
	ImGui::Text("%s", a_entry.feature->GetName().c_str());
	ImGui::PopStyleColor();

	ImGui::Spacing();

	// Description
	ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
	ImGui::TextWrapped("%s", a_entry.description);
	ImGui::PopStyleColor();

	ImGui::Spacing();

	// Performance badge
	{
		ImVec4 badgeColor;
		const char* perfLabel;
		switch (a_entry.perfLevel) {
		case 2:
			badgeColor = kColorPerfHeavy;
			perfLabel = "Heavy";
			break;
		case 1:
			badgeColor = kColorPerfMedium;
			perfLabel = "Medium";
			break;
		default:
			badgeColor = kColorPerfLight;
			perfLabel = "Light";
			break;
		}
		ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
		ImGui::Text("Performance: %s %s", perfLabel, a_entry.badge);
		ImGui::PopStyleColor();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Enable/disable toggle
	if (DrawToggle("Enabled", &enabled)) {
		a_entry.feature->loaded = enabled;
		if (!enabled)
			userToggledOff.insert(a_entry.feature->GetShortName());
		else
			userToggledOff.erase(a_entry.feature->GetShortName());
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Per-feature settings (scrollable)
	const float bottomBarHeight = ImGui::GetFrameHeightWithSpacing() * 3.5f;
	ImGui::BeginChild("##FeatureSettings", ImVec2(0, -bottomBarHeight), ImGuiChildFlags_NavFlattened);

	if (enabled) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
		ImGui::Text("Settings");
		ImGui::PopStyleColor();
		ImGui::Spacing();
		a_entry.feature->DrawSettings();
	} else {
		ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
		ImGui::TextWrapped("Enable this feature to access its settings.");
		ImGui::PopStyleColor();
	}

	ImGui::EndChild();
}

void BasicMenu::DrawInputHints()
{
	ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);

	if (inputMode == InputMode::kGamepad) {
		if (selectedFeatureIdx >= 0) {
			ImGui::Text("[B] Back    [A] Toggle    [LB/RB] Adjust");
		} else {
			ImGui::Text("[A] Select    [B] Close    [LB/RB] Page Scroll");
		}
	} else {
		auto* menu = Menu::GetSingleton();
		std::string keyName = menu ? Util::Input::KeyIdToString(menu->GetSettings().ToggleKey) : "END";
		if (selectedFeatureIdx >= 0) {
			ImGui::Text("Press %s for advanced settings", keyName.c_str());
		} else {
			ImGui::Text("Click feature for details | %s for advanced settings", keyName.c_str());
		}
	}

	ImGui::PopStyleColor();
}

void BasicMenu::DrawVRAM()
{
	auto menu = Menu::GetSingleton();
	if (!menu)
		return;
	auto dxgiAdapter3 = menu->GetDXGIAdapter3();
	if (!dxgiAdapter3)
		return;

	DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo{};
	HRESULT hr = dxgiAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo);

	if (SUCCEEDED(hr) && videoMemoryInfo.Budget > 0) {
		float usedGB = videoMemoryInfo.CurrentUsage / (1024.f * 1024.f * 1024.f);
		float totalGB = videoMemoryInfo.Budget / (1024.f * 1024.f * 1024.f);
		float percent = usedGB / totalGB;

		// Color based on usage
		ImVec4 barColor = percent < 0.7f ? kColorVRAMOk : percent < 0.9f ? kColorVRAMWarn :
		                                                                    kColorVRAMCrit;

		std::string label = std::format("VRAM  {:.1f} / {:.1f} GB", usedGB, totalGB);
		ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
		ImGui::TextUnformatted(label.c_str());
		ImGui::PopStyleColor();

		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
		ImGui::ProgressBar(percent, ImVec2(-1, 0), "");
		ImGui::PopStyleColor();
	}
}