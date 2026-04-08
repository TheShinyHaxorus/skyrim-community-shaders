#include "BasicMenu.h"

#include "Feature.h"
#include "FeatureCategories.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"
#include "Menu.h"
#include "State.h"

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

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
// Flash dispatches by event.index (hardcoded switch).  We insert our entry
// into the visual list and wrap the Flash handler so that:
//   - Clicks on our entry activate BasicMenu (index == csEntryIndex)
//   - Clicks on entries AFTER ours get index-1 before forwarding to the
//     original handler, undoing the insertion shift
//   - Clicks on entries BEFORE ours forwarded unchanged

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
		// Our entry was clicked — close journal, activate BasicMenu
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
	if (auto it = std::ranges::find(elements, csLabel); it != elements.end()) {
		csEntryIndex = static_cast<std::int32_t>(std::distance(elements.begin(), it));
		return true;
	}

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
	csEntryIndex = static_cast<std::int32_t>(insertPos);

	// Rebuild the GFx entry list
	entryList.ClearElements();
	for (auto& text : elements) {
		RE::GFxValue entry;
		view->CreateObject(&entry);
		entry.SetMember("text", text.c_str());
		entryList.PushBack(entry);
	}
	categoryList.Invoke("InvalidateData");

	// Wrap the Flash item-press handler so we can intercept our entry
	// and adjust shifted indices for all others.
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
		journalHooked = SetupJournalMenu();
	}

	return RE::BSEventNotifyControl::kContinue;
}

// --- Activation / Deactivation ---

void BasicMenu::Activate()
{
	RebuildFeatureList();
	firstFrame = true;
	active = true;
}

void BasicMenu::Deactivate()
{
	if (!active)
		return;

	active = false;

	// Persist changes
	globals::state->Save();
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

// Style — lower alpha for transparency so the game world is visible
static constexpr float kBgAlpha = 0.35f;
static constexpr float kBorderSize = 2.0f;

static constexpr ImVec4 kColorBg{ 0.0f, 0.0f, 0.0f, kBgAlpha };
static constexpr ImVec4 kColorBorder{ 0.5f, 0.5f, 0.5f, 0.4f };
static constexpr ImVec4 kColorText{ 1.0f, 1.0f, 1.0f, 1.0f };
static constexpr ImVec4 kColorTextDim{ 0.7f, 0.7f, 0.7f, 0.8f };
static constexpr ImVec4 kColorFrameBg{ 0.15f, 0.15f, 0.15f, 0.5f };
static constexpr ImVec4 kColorFrameBgHovered{ 0.25f, 0.25f, 0.25f, 0.6f };
static constexpr ImVec4 kColorCheckMark{ 1.0f, 1.0f, 1.0f, 1.0f };
static constexpr ImVec4 kColorSliderGrab{ 1.0f, 1.0f, 1.0f, 0.3f };
static constexpr ImVec4 kColorSliderGrabActive{ 1.0f, 1.0f, 1.0f, 0.6f };
static constexpr ImVec4 kColorButton{ 0.15f, 0.15f, 0.15f, 0.5f };
static constexpr ImVec4 kColorButtonHovered{ 0.3f, 0.3f, 0.3f, 0.7f };
static constexpr ImVec4 kColorButtonActive{ 0.4f, 0.4f, 0.4f, 0.8f };
static constexpr ImVec4 kColorNavHighlight{ 1.0f, 1.0f, 1.0f, 0.4f };

// Performance badge colors
static constexpr ImVec4 kColorPerfHeavy{ 1.0f, 0.35f, 0.35f, 1.0f };
static constexpr ImVec4 kColorPerfMedium{ 1.0f, 0.8f, 0.3f, 1.0f };
static constexpr ImVec4 kColorPerfLight{ 0.5f, 0.9f, 0.5f, 1.0f };

// VRAM bar colors
static constexpr ImVec4 kColorVRAMOk{ 0.3f, 0.8f, 0.3f, 1.0f };
static constexpr ImVec4 kColorVRAMWarn{ 1.0f, 0.8f, 0.2f, 1.0f };
static constexpr ImVec4 kColorVRAMCrit{ 1.0f, 0.3f, 0.3f, 1.0f };

static constexpr int kStyleColorCount = 17;
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
	ImGui::PushStyleColor(ImGuiCol_CheckMark, kColorCheckMark);
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, kColorSliderGrab);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kColorSliderGrabActive);
	ImGui::PushStyleColor(ImGuiCol_Button, kColorButton);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorButtonHovered);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorButtonActive);
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.15f, 0.15f, 0.4f));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.5f));
	ImGui::PushStyleColor(ImGuiCol_NavCursor, kColorNavHighlight);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, kBorderSize);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
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
	io.MouseDrawCursor = true;

	// ESC / gamepad B → close
	if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
		Deactivate();
		return;
	}

	PushPanelStyle();

	const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
	const float panelWidth = vpSize.x * 0.28f;
	const float panelHeight = vpSize.y * 0.80f;
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
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.9f));
	ImGui::Text("COMMUNITY SHADERS");
	ImGui::PopStyleColor();
	ImGui::Separator();
	ImGui::Spacing();

	// Quality preset bar
	{
		ImGui::Text("Quality Preset");
		ImGui::Spacing();

		// Seed gamepad focus on the first interactive widget
		if (firstFrame)
			ImGui::SetKeyboardFocusHere();

		static constexpr const char* presetLabels[] = { "Low", "Medium", "High" };
		const float btnWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

		for (int i = 0; i < 3; ++i) {
			if (i > 0)
				ImGui::SameLine();
			if (ImGui::Button(presetLabels[i], ImVec2(btnWidth, 0))) {
				// Apply to SSGI
				auto& ssgi = globals::features::screenSpaceGI;
				if (ssgi.loaded) {
					switch (i) {
					case 0:  // Low
						ssgi.settings.NumSlices = 10;
						ssgi.settings.NumSteps = 12;
						ssgi.settings.ResolutionMode = 2;
						ssgi.settings.EnableBlur = true;
						ssgi.settings.EnableGI = true;
						ssgi.recompileFlag = true;
						break;
					case 1:  // Medium
						ssgi.settings.NumSlices = 4;
						ssgi.settings.NumSteps = 8;
						ssgi.settings.ResolutionMode = 1;
						ssgi.settings.EnableBlur = true;
						ssgi.settings.EnableGI = true;
						ssgi.recompileFlag = true;
						break;
					case 2:  // High
						ssgi.settings.NumSlices = 4;
						ssgi.settings.NumSteps = 8;
						ssgi.settings.ResolutionMode = 0;
						ssgi.settings.EnableBlur = true;
						ssgi.settings.EnableGI = true;
						ssgi.recompileFlag = true;
						break;
					}
				}

				// Apply to Volumetric Lighting
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

	// Scrollable feature toggle list
	const float vramBarHeight = ImGui::GetFrameHeightWithSpacing() * 3.0f;
	ImGui::BeginChild("##FeatureList", ImVec2(0, -vramBarHeight), ImGuiChildFlags_NavFlattened);

	for (auto& entry : featureList)
		DrawFeatureToggle(entry);

	ImGui::EndChild();

	if (firstFrame)
		firstFrame = false;

	// VRAM bar pinned at bottom
	ImGui::Separator();
	DrawVRAM();

	// Hint
	ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
	ImGui::Text("Press END for advanced settings");
	ImGui::PopStyleColor();

	ImGui::End();
	PopPanelStyle();
}

void BasicMenu::DrawFeatureToggle(const FeatureEntry& a_entry)
{
	ImGui::PushID(a_entry.feature->GetShortName().c_str());

	// Runtime toggle — directly controls whether the feature is active
	bool enabled = a_entry.feature->loaded;

	if (ImGui::Checkbox(a_entry.feature->GetName().c_str(), &enabled)) {
		a_entry.feature->loaded = enabled;
		if (!enabled)
			userToggledOff.insert(a_entry.feature->GetShortName());
		else
			userToggledOff.erase(a_entry.feature->GetShortName());
	}

	// Color-coded resource badge on the same line, right-aligned
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

		ImGui::SameLine();
		float textWidth = ImGui::CalcTextSize(a_entry.badge).x;
		ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - textWidth);
		ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
		ImGui::TextUnformatted(a_entry.badge);
		ImGui::PopStyleColor();
	}

	// One-line description
	ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
	ImGui::TextWrapped("  %s", a_entry.description);
	ImGui::PopStyleColor();

	ImGui::Spacing();
	ImGui::PopID();
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
