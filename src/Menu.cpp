#include "Menu.h"

#ifndef DIRECTINPUT_VERSION
#	define DIRECTINPUT_VERSION 0x0800
#endif
#include <algorithm>
#include <cmath>
#include <dinput.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <iomanip>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "Deferred.h"
#include "Feature.h"
#include "FeatureCategories.h"
#include "FeatureIssues.h"
#include "FeatureVersions.h"
#include "Features/Upscaling.h"
#include "Menu/AdvancedSettingsRenderer.h"
#include "Menu/BackgroundBlur.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/Fonts.h"
#include "Menu/HomePageRenderer.h"
#include "Menu/IconLoader.h"
#include "Menu/MenuHeaderRenderer.h"
#include "Menu/OverlayRenderer.h"
#include "Menu/SettingsTabRenderer.h"
#include "Menu/ThemeManager.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Util.h"
#include "Utils/UI.h"

#include "Features/PerformanceOverlay.h"
#include "Features/PerformanceOverlay/ABTesting/ABTestAggregator.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#include "Features/VR.h"
#include "Features/WeatherEditor.h"
#include "WeatherEditor/EditorWindow.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Menu::ThemeSettings::PaletteColors,
	Background,
	Text,
	WindowBorder,
	FrameBorder,
	Separator,
	ResizeGrip)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Menu::ThemeSettings::StatusPaletteColors,
	Disable,
	Error,
	Warning,
	RestartNeeded,
	CurrentHotkey,
	SuccessColor,
	InfoColor)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Menu::ThemeSettings::FeatureHeadingColors,
	ColorDefault,
	ColorHovered,
	MinimizedFactor,
	FeatureTitleScale)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Menu::ThemeSettings::ScrollbarOpacitySettings,
	Background,
	Thumb,
	ThumbHovered,
	ThumbActive)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Menu::ThemeSettings::FontRoleSettings,
	Family,
	Style,
	File,
	SizeScale)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ImGuiStyle,
	WindowPadding,
	WindowRounding,
	WindowBorderSize,
	WindowMinSize,
	ChildRounding,
	ChildBorderSize,
	PopupRounding,
	PopupBorderSize,
	FramePadding,
	FrameRounding,
	FrameBorderSize,
	ItemSpacing,
	ItemInnerSpacing,
	CellPadding,
	IndentSpacing,
	ColumnsMinSpacing,
	ScrollbarSize,
	ScrollbarRounding,
	GrabMinSize,
	GrabRounding,
	LogSliderDeadzone,
	ImageRounding,
	ImageBorderSize,
	TabRounding,
	TabBorderSize,
	TabCloseButtonMinWidthSelected,
	TabCloseButtonMinWidthUnselected,
	TabBarBorderSize,
	TabBarOverlineSize,
	TableAngledHeadersAngle,
	TableAngledHeadersTextAlign,
	TreeLinesSize,
	TreeLinesRounding,
	DragDropTargetRounding,
	DragDropTargetBorderSize,
	DragDropTargetPadding,
	ColorMarkerSize,
	ColorButtonPosition,
	ButtonTextAlign,
	SelectableTextAlign,
	SeparatorTextBorderSize,
	SeparatorTextAlign,
	SeparatorTextPadding,
	DockingSeparatorSize,
	MouseCursorScale)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Menu::ThemeSettings,
	FontSize,
	FontName,
	GlobalScale,
	FontRoles,
	UseSimplePalette,
	ShowActionIcons,
	UseMonochromeIcons,
	UseMonochromeLogo,
	ShowFooter,
	CenterHeader,
	TooltipHoverDelay,
	BackgroundBlurEnabled,
	ScrollbarOpacity,
	Palette,
	StatusPalette,
	FeatureHeading,
	Style)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Menu::Settings,
	ToggleKey,
	SkipCompilationKey,
	EffectToggleKey,
	OverlayToggleKey,
	ShaderBlockPrevKey,
	ShaderBlockNextKey,
	WeatherEditorToggleKey,
	EnableShaderBlocking,
	FirstTimeSetupCompleted,
	SkipClearCacheConfirmation,
	AutoHideFeatureList,
	SkipConstraintWarning,
	RequireShiftToDock,
	UseResolutionFont,
	Theme,
	SelectedThemePreset)

bool IsEnabled = false;
std::unordered_map<std::string, int> Menu::categoryCounts;

// Pad FontRoles JSON array with defaults if shorter than FontRole::Count.
// Prevents deserialization failure when loading old settings with fewer font roles.
static void SanitizeFontRolesJson(json& themeJson)
{
	if (!themeJson.contains("FontRoles") || !themeJson["FontRoles"].is_array())
		return;

	auto& fontRoles = themeJson["FontRoles"];
	const size_t expected = static_cast<size_t>(Menu::FontRole::Count);

	if (fontRoles.size() < expected) {
		auto defaults = Menu::ThemeSettings{}.FontRoles;
		for (size_t i = fontRoles.size(); i < expected; ++i) {
			fontRoles.push_back(defaults[i]);
		}
	}
}

// Serialize palette as named color map. Resilient to ImGui enum reordering.
void Menu::PaletteToJson(json& themeJson, const std::array<ImVec4, ImGuiCol_COUNT>& palette)
{
	json colors = json::object();
	for (int i = 0; i < ImGuiCol_COUNT; i++)
		colors[ImGui::GetStyleColorName(i)] = palette[i];
	themeJson["Colors"] = colors;
}

// Deserialize palette from named color map (preferred) or legacy positional array.
void Menu::PaletteFromJson(const json& themeJson, std::array<ImVec4, ImGuiCol_COUNT>& palette)
{
	ThemeSettings defaults;
	palette = defaults.FullPalette;

	auto loadVec4 = [](const json& c) -> ImVec4 {
		if (c.is_array() && c.size() >= 4)
			return c.get<ImVec4>();
		return ImVec4(0, 0, 0, 0);
	};

	if (themeJson.contains("Colors") && themeJson["Colors"].is_object()) {
		// Named color map: look up each color by ImGui's style color name
		const auto& colors = themeJson["Colors"];
		for (int i = 0; i < ImGuiCol_COUNT; i++) {
			const char* name = ImGui::GetStyleColorName(i);
			if (colors.contains(name) && colors[name].is_array())
				palette[i] = loadVec4(colors[name]);
		}
	} else if (themeJson.contains("FullPalette") && themeJson["FullPalette"].is_array()) {
		// Legacy positional array
		const auto& arr = themeJson["FullPalette"];

		if (arr.size() == 55) {
			// Migrate from ImGui 1.90 (55 entries) to 1.92+ (62 entries).
			// Tab/TabHovered swapped, 7 new slots inserted mid-enum.
			for (int i = 0; i <= 32; i++)
				palette[i] = loadVec4(arr[i]);
			// [33] InputTextCursor: stays default
			palette[34] = loadVec4(arr[34]);  // old TabHovered → TabHovered
			palette[35] = loadVec4(arr[33]);  // old Tab → Tab (swapped)
			palette[36] = loadVec4(arr[35]);  // old TabActive → TabSelected
			// [37] TabSelectedOverline: stays default
			palette[38] = loadVec4(arr[36]);  // old TabUnfocused → TabDimmed
			palette[39] = loadVec4(arr[37]);  // old TabUnfocusedActive → TabDimmedSelected
			// [40] TabDimmedSelectedOverline: stays default
			for (int i = 38; i <= 48; i++)
				palette[i + 3] = loadVec4(arr[i]);
			// [52] TextLink: stays default
			palette[53] = loadVec4(arr[49]);  // TextSelectedBg
			// [54] TreeLines: stays default
			palette[55] = loadVec4(arr[50]);  // DragDropTarget
			// [56] DragDropTargetBg: stays default
			// [57] UnsavedMarker: stays default
			for (int i = 51; i <= 54; i++)
				palette[i + 7] = loadVec4(arr[i]);
		} else {
			// Direct positional load (matching or close size)
			size_t count = std::min(arr.size(), static_cast<size_t>(ImGuiCol_COUNT));
			for (size_t i = 0; i < count; i++)
				palette[i] = loadVec4(arr[i]);
		}
	}
}

std::optional<Menu::FontRole> Menu::ResolveFontRole(std::string_view key)
{
	for (size_t i = 0; i < FontRoleDescriptors.size(); ++i) {
		if (FontRoleDescriptors[i].key == key) {
			return static_cast<FontRole>(i);
		}
	}
	return std::nullopt;
}

std::string Menu::BuildFontSignature(float baseFontSize) const
{
	return MenuFonts::BuildFontSignature(settings.Theme, baseFontSize);
}

const Menu::ThemeSettings::FontRoleSettings& Menu::GetDefaultFontRole(FontRole role)
{
	return MenuFonts::GetDefaultRole(role);
}

Menu::~Menu()
{  // Release icon textures if loaded
	uiIcons.saveSettings.Release();
	uiIcons.loadSettings.Release();
	uiIcons.deleteSettings.Release();
	uiIcons.clearCache.Release();
	uiIcons.logo.Release();
	uiIcons.featureSettingRevert.Release();
	uiIcons.applyToGame.Release();
	uiIcons.pauseTime.Release();
	uiIcons.undo.Release();
	uiIcons.discord.Release();
	uiIcons.characters.Release();
	uiIcons.display.Release();
	uiIcons.grass.Release();
	uiIcons.lighting.Release();
	uiIcons.sky.Release();
	uiIcons.landscape.Release();
	uiIcons.water.Release();
	uiIcons.debug.Release();
	uiIcons.materials.Release();
	uiIcons.postProcessing.Release();
	uiIcons.freeCamera.Release();
	uiIcons.playMode.Release();
	uiIcons.search.Release();

	// Clean up blur resources
	BackgroundBlur::Cleanup();

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	dxgiAdapter3 = nullptr;
}

void Menu::Load(json& o_json)
{
	// Store current Theme state before loading config
	auto currentTheme = settings.Theme;

	settings = o_json;

	// Restore Theme - don't load it from config, only from theme preset files
	settings.Theme = currentTheme;

	// Migration: Convert legacy uint32_t keys to InputCombo vectors if needed
	auto migrateKey = [](json& j, const char* keyName, std::vector<InputCombo>& target) {
		if (j.contains(keyName) && j[keyName].is_number_integer()) {
			uint32_t legacyKey = j[keyName].get<uint32_t>();
			target.clear();
			if (legacyKey != 0) {
				target.push_back(InputCombo::Keyboard(legacyKey));
			}
		}
	};

	migrateKey(o_json, "ToggleKey", settings.ToggleKey);
	migrateKey(o_json, "SkipCompilationKey", settings.SkipCompilationKey);
	migrateKey(o_json, "EffectToggleKey", settings.EffectToggleKey);
	migrateKey(o_json, "OverlayToggleKey", settings.OverlayToggleKey);
	migrateKey(o_json, "ShaderBlockPrevKey", settings.ShaderBlockPrevKey);
	migrateKey(o_json, "ShaderBlockNextKey", settings.ShaderBlockNextKey);
	migrateKey(o_json, "WeatherEditorToggleKey", settings.WeatherEditorToggleKey);

	// Helper for new smart serialization with error handling
	auto loadComboList = [](const json& j, const char* keyName, std::vector<InputCombo>& target) {
		if (j.contains(keyName) && j[keyName].is_array()) {
			try {
				InputCombo::ComboList::from_json(j[keyName], target);
			} catch (const std::exception& e) {
				logger::warn("Failed to load combo list '{}': {}, using default", keyName, e.what());
				// Leave target unchanged (keeps default or migrated value)
			}
		}
	};

	loadComboList(o_json, "ToggleKey", settings.ToggleKey);
	loadComboList(o_json, "SkipCompilationKey", settings.SkipCompilationKey);
	loadComboList(o_json, "EffectToggleKey", settings.EffectToggleKey);
	loadComboList(o_json, "OverlayToggleKey", settings.OverlayToggleKey);
	loadComboList(o_json, "ShaderBlockPrevKey", settings.ShaderBlockPrevKey);
	loadComboList(o_json, "ShaderBlockNextKey", settings.ShaderBlockNextKey);
	loadComboList(o_json, "WeatherEditorToggleKey", settings.WeatherEditorToggleKey);

	// Legacy support: If old config has Theme data and no SelectedThemePreset, load it
	if (o_json.contains("Theme") && o_json["Theme"].is_object() && settings.SelectedThemePreset.empty()) {
		bool hasFontRoles = o_json["Theme"].contains("FontRoles");
		SanitizeFontRolesJson(o_json["Theme"]);
		settings.Theme = o_json["Theme"];
		PaletteFromJson(o_json["Theme"], settings.Theme.FullPalette);
		MenuFonts::NormalizeFontRoles(settings.Theme, hasFontRoles);

		auto& bodyRole = settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)];
		if (!Util::ValidateFont(bodyRole.File)) {
			const auto& defaults = Menu::GetDefaultFontRole(FontRole::Body);
			logger::warn("Font '{}' not found while loading settings, falling back to default font '{}'",
				bodyRole.File, defaults.File);
			settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)] = defaults;
			settings.Theme.FontName = defaults.File;
		}
		logger::info("Loaded legacy Theme data from config (no SelectedThemePreset)");
	}

	// Apply Default Dark theme on first launch if no theme is selected
	if (!settings.FirstTimeSetupCompleted && settings.SelectedThemePreset.empty()) {
		// Ensure default themes are created/available
		CreateDefaultThemes();

		// Load the Default Dark theme and mark it as selected to prevent override
		if (LoadThemePreset("Default")) {
			settings.SelectedThemePreset = "Default";  // Mark as selected to prevent State::LoadTheme override
			logger::info("Applied Default Dark theme on first launch");
		} else {
			logger::warn("Failed to load Default Dark theme on first launch");
		}
	} else if (!settings.SelectedThemePreset.empty()) {
		// Load the previously selected theme preset (including custom themes)
		if (LoadThemePreset(settings.SelectedThemePreset)) {
			logger::info("Loaded saved theme preset: {}", settings.SelectedThemePreset);
		} else {
			logger::warn("Failed to load saved theme preset '{}', falling back to Default", settings.SelectedThemePreset);
			if (LoadThemePreset("Default")) {
				settings.SelectedThemePreset = "Default";
			}
		}
	}
}

void Menu::Save(json& o_json)
{
	settings.Theme.FontName = settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)].File;

	// Save all settings except Theme values
	// Theme values should only be saved in theme preset files, not in the main config
	o_json = settings;

	// Remove Theme object from config, only keep SelectedThemePreset
	o_json.erase("Theme");

	// Manually save input combos using the smart serializer
	InputCombo::ComboList::to_json(o_json["ToggleKey"], settings.ToggleKey);
	InputCombo::ComboList::to_json(o_json["SkipCompilationKey"], settings.SkipCompilationKey);
	InputCombo::ComboList::to_json(o_json["EffectToggleKey"], settings.EffectToggleKey);
	InputCombo::ComboList::to_json(o_json["OverlayToggleKey"], settings.OverlayToggleKey);
	InputCombo::ComboList::to_json(o_json["ShaderBlockPrevKey"], settings.ShaderBlockPrevKey);
	InputCombo::ComboList::to_json(o_json["ShaderBlockNextKey"], settings.ShaderBlockNextKey);
	InputCombo::ComboList::to_json(o_json["WeatherEditorToggleKey"], settings.WeatherEditorToggleKey);
}

void Menu::LoadTheme(json& o_json)
{
	if (o_json["Theme"].is_object()) {
		bool hasFontRoles = o_json["Theme"].contains("FontRoles");
		SanitizeFontRolesJson(o_json["Theme"]);
		settings.Theme = o_json["Theme"];
		PaletteFromJson(o_json["Theme"], settings.Theme.FullPalette);
		MenuFonts::NormalizeFontRoles(settings.Theme, hasFontRoles);

		auto& bodyRole = settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)];
		if (!Util::ValidateFont(bodyRole.File)) {
			const auto& defaults = Menu::GetDefaultFontRole(FontRole::Body);
			logger::warn("Font '{}' not found, falling back to default font '{}'",
				bodyRole.File, defaults.File);
			settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)] = defaults;
			settings.Theme.FontName = defaults.File;
		}

		// Apply background blur enabled state from theme
		BackgroundBlur::SetEnabled(settings.Theme.BackgroundBlurEnabled);
	}
}
void Menu::SaveTheme(json& o_json)
{
	settings.Theme.FontName = settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)].File;

	if (!Util::ValidateFont(settings.Theme.FontName)) {
		const auto& defaults = Menu::GetDefaultFontRole(FontRole::Body);
		logger::warn("Font '{}' not found during save, falling back to default font '{}'",
			settings.Theme.FontName, defaults.File);
		settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)] = defaults;
		settings.Theme.FontName = defaults.File;
	}

	o_json["Theme"] = settings.Theme;
	PaletteToJson(o_json["Theme"], settings.Theme.FullPalette);
}

std::vector<std::string> Menu::DiscoverThemes()
{
	auto themeManager = ThemeManager::GetSingleton();
	if (themeManager) {
		themeManager->DiscoverThemes();
		return themeManager->GetThemeNames();
	}
	return {};
}

bool Menu::LoadThemePreset(const std::string& themeName)
{
	if (themeName.empty()) {
		// Empty theme name means custom/user theme
		settings.SelectedThemePreset = "";
		return true;
	}

	auto themeManager = ThemeManager::GetSingleton();
	json themeSettings;

	if (themeManager->LoadTheme(themeName, themeSettings)) {
		// Create a backup of current theme in case loading fails
		ThemeSettings backupTheme = settings.Theme;
		bool hasFontRoles = themeSettings.contains("FontRoles");

		SanitizeFontRolesJson(themeSettings);

		try {
			settings.Theme = themeSettings;
			PaletteFromJson(themeSettings, settings.Theme.FullPalette);

			MenuFonts::NormalizeFontRoles(settings.Theme, hasFontRoles);
			auto& bodyRole = settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)];
			if (!Util::ValidateFont(bodyRole.File)) {
				const auto& defaults = Menu::GetDefaultFontRole(FontRole::Body);
				logger::warn("Font '{}' from theme '{}' not found, falling back to default font '{}'",
					bodyRole.File, themeName, defaults.File);
				settings.Theme.FontRoles[static_cast<size_t>(FontRole::Body)] = defaults;
				settings.Theme.FontName = defaults.File;
			}

			settings.SelectedThemePreset = themeName;

			// Schedule deferred font reload if font has changed
			if (settings.Theme.FontName != cachedFontName) {
				pendingFontReload = true;
			}

			// Schedule deferred icon reload to apply theme-specific icon overrides
			pendingIconReload = true;

			// Apply background blur enabled state from theme
			BackgroundBlur::SetEnabled(settings.Theme.BackgroundBlurEnabled);

			logger::info("Applied theme preset: {}", themeName);
			return true;
		} catch (const std::exception& e) {
			logger::warn("Error loading theme '{}': {}", themeName, e.what());
			settings.Theme = backupTheme;
			return false;
		}
	} else {
		logger::warn("Failed to load theme preset: {}", themeName);
		return false;
	}
}

void Menu::CreateDefaultThemes()
{
	auto themeManager = ThemeManager::GetSingleton();
	themeManager->CreateDefaultThemeFiles();
}

void Menu::Init()
{
	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	// IMPORTANT: Immediately override ImGui's default styles with our Default.json theme
	// This prevents hardcoded ImGui defaults from ever showing through
	auto* themeManager = ThemeManager::GetSingleton();
	json defaultThemeSettings;
	if (themeManager->LoadTheme("Default", defaultThemeSettings)) {
		// Temporarily create a minimal theme structure to apply defaults
		json tempSettings;
		tempSettings["Theme"] = defaultThemeSettings;
		LoadTheme(tempSettings);
		logger::info("Applied Default.json theme immediately after ImGui context creation");
	} else {
		logger::warn("Could not load Default.json theme - trying direct force application");
		// Last resort: Apply Default.json colors directly to ImGui
		ThemeManager::ForceApplyDefaultTheme();
	}

	// Re-apply user-selected preset after defaults are applied (covers Default and custom)
	if (!settings.SelectedThemePreset.empty()) {
		auto themeManagerSingleton = ThemeManager::GetSingleton();
		if (themeManagerSingleton && !themeManagerSingleton->IsDiscovered()) {
			themeManagerSingleton->DiscoverThemes();
		}

		if (!LoadThemePreset(settings.SelectedThemePreset)) {
			logger::warn("Failed to re-apply preset '{}' during Menu::Init. Keeping Default.", settings.SelectedThemePreset);
		} else {
			logger::info("Re-applied preset '{}' during Menu::Init", settings.SelectedThemePreset);
		}
	}

	auto& imgui_io = ImGui::GetIO();
	imgui_io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_DockingEnable;
	imgui_io.ConfigDockingWithShift = settings.RequireShiftToDock;
	imgui_io.BackendFlags = ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_HasGamepad;

	cachedIniPath = Util::PathHelpers::GetImGuiIniPath().string();
	imgui_io.IniFilename = cachedIniPath.c_str();

	// Register settings handler to persist display size for cross-session resolution change detection
	ImGuiSettingsHandler handler{};
	handler.TypeName = "CommunityShaders";
	handler.TypeHash = ImHashStr("CommunityShaders");
	handler.UserData = &lastDisplaySize;
	handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* { return (void*)1; };
	handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* h, void*, const char* line) {
		float w, ht;
		if (sscanf(line, "DisplaySize=%f,%f", &w, &ht) == 2)
			*static_cast<float2*>(h->UserData) = { w, ht };
	};
	handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
		auto& ds = ImGui::GetIO().DisplaySize;
		buf->appendf("[%s][Data]\nDisplaySize=%g,%g\n\n", h->TypeName, ds.x, ds.y);
	};
	ImGui::GetCurrentContext()->SettingsHandlers.push_back(handler);

	DXGI_SWAP_CHAIN_DESC desc{};
	globals::d3d::swapChain->GetDesc(&desc);

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(desc.OutputWindow);
	ImGui_ImplDX11_Init(globals::d3d::device, globals::d3d::context);

	ThemeManager::ReloadFont(*this, cachedFontSize);

	{
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (!FAILED(globals::d3d::device->QueryInterface(dxgiDevice.put()))) {
			winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
			if (!FAILED(dxgiDevice->GetAdapter(dxgiAdapter.put()))) {
				dxgiAdapter->QueryInterface(dxgiAdapter3.put());
			}
		}
	}
	// Load UI icons
	if (!Util::InitializeMenuIcons(this)) {
		logger::warn("Menu::Init() - Failed to load UI icons. Will fallback to text buttons");
	}

	// Initialize background blur system
	if (!BackgroundBlur::Initialize()) {
		logger::warn("Menu::Init() - Failed to initialize background blur system");
	}

	BuildCategoryCounts();

	initialized = true;
}

/**
 * @brief Main UI rendering coordinator for the Community Shaders menu
 *
 * This method serves as the primary entry point for rendering the entire menu interface.
 * It handles window setup, docking configuration, and delegates rendering to specialized
 * renderer components for better separation of concerns.
 *
 * The method manages:
 * - ImGui docking space and window positioning
 * - Focus change handling
 * - Dynamic window flags based on docking state
 * - Header, navigation tabs, and settings panels coordination
 */
void Menu::DrawSettings()
{
	if (focusChanged) {
		OnFocusChanged();
		focusChanged = false;
	}

	// Apply theme styling with universal contrast enhancement
	ThemeManager::SetupImGuiStyle(*this);

	ImGui::DockSpaceOverViewport(0, NULL, ImGuiDockNodeFlags_PassthruCentralNode);

	const auto layoutCond = resetLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
	ImGui::SetNextWindowPos(Util::GetNativeViewportSizeScaled(0.5f), layoutCond, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(Util::GetNativeViewportSizeScaled(0.8f), layoutCond);
	resetLayout = false;
	auto versionStr = Util::GetFormattedVersion(Plugin::VERSION);
	auto expectedTag = std::format("v{}", versionStr);
	auto displayTitle = Plugin::BUILD_DESCRIBE == expectedTag ? std::format("Community Shaders {}", versionStr) : std::format("Community Shaders {} [{}]", versionStr, Plugin::BUILD_DESCRIBE);
	// Use ### to keep a stable window ID regardless of build suffix, preserving docking state
	auto title = std::format("{}###CommunityShaders", displayTitle);

	// Determine window flags based on docking state
	ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
	// Check if this will be docked (we need to peek at the docking state)
	static bool wasDocked = false;
	bool willBeDocked = wasDocked;  // Use previous frame's state as approximation

	// Only hide title bar when not docked
	if (!willBeDocked) {
		windowFlags |= ImGuiWindowFlags_NoTitleBar;
	}

	ImGui::Begin(title.c_str(), &IsEnabled, windowFlags);
	{
		// Update docking state tracking
		bool isDocked = ImGui::IsWindowDocked();
		wasDocked = isDocked;

		float globalScale = settings.Theme.GlobalScale;

		// Use default global scale (0.0) for built-in themes when GlobalScale equals the default
		if (std::abs(globalScale - ThemeManager::Constants::DEFAULT_GLOBAL_SCALE) < 0.001f) {
			globalScale = ThemeManager::Constants::DEFAULT_GLOBAL_SCALE;  // Ensure built-in themes stay at 0.0
		}

		const float uiScale = exp2(globalScale);  // User's manual GlobalScale for header icons
		// Check if we can show icons - require setting enabled and at least some icons loaded (for undocked)
		// For docked mode, always show icons if textures are available
		bool canShowIcons = settings.Theme.ShowActionIcons &&
		                    (uiIcons.saveSettings.texture ||
								uiIcons.loadSettings.texture ||
								uiIcons.clearCache.texture);  // Always show logo if available, regardless of action icons setting
		bool showLogo = uiIcons.logo.texture != nullptr;

		// Render header using extracted component
		MenuHeaderRenderer::RenderHeader(isDocked, showLogo, canShowIcons, uiScale, uiIcons);

		// Main content starts here - no additional separator needed as it's already handled in the conditions above

		float footer_height = settings.Theme.ShowFooter ?
		                          (ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 3) :
		                          0.0f;

		// Static storage for menu state - must persist across frames
		static size_t selectedMenu = 0;
		static std::map<std::string, bool> categoryExpansionStates;

		// Render feature list using extracted component
		FeatureListRenderer::RenderFeatureList(
			footer_height,
			selectedMenu,
			featureSearch,
			pendingFeatureSelection,
			categoryExpansionStates,
			[&]() { DrawGeneralSettings(); },
			[&]() { DrawAdvancedSettings(); });

		if (settings.Theme.ShowFooter) {
			ImGui::Spacing();
			ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS);
			ImGui::Spacing();
			DrawFooter();
		}

		// Draw global popups (needs to be called once per frame)
		Util::DrawClearShaderCacheConfirmation();
	}
	ImGui::End();
}

/**
 * @brief Renders the General settings tab content
 *
 * Delegates rendering to SettingsTabRenderer for the general configuration panel,
 * which includes Shaders, Keybindings, and Interface sub-tabs. This method provides
 * the callback for key-to-string conversion while maintaining separation of concerns.
 */
void Menu::DrawGeneralSettings()
{
	// Prepare settings state for the renderer
	SettingsTabRenderer::SettingsState state{
		.settingToggleKey = settingToggleKey,
		.settingsEffectsToggle = settingsEffectsToggle,
		.settingSkipCompilationKey = settingSkipCompilationKey,
		.settingOverlayToggleKey = settingOverlayToggleKey,
		.settingShaderBlockPrevKey = settingShaderBlockPrevKey,
		.settingShaderBlockNextKey = settingShaderBlockNextKey,
		.settingWeatherEditorToggleKey = settingWeatherEditorToggleKey
	};

	// Render settings using extracted component
	SettingsTabRenderer::RenderGeneralSettings(state);
}

/**
 * @brief Renders the Advanced settings tab content
 *
 * Delegates rendering to AdvancedSettingsRenderer for developer and advanced user
 * settings. Uses lambda callbacks to access private Menu methods while maintaining
 * encapsulation and proper separation of concerns.
 */
void Menu::DrawAdvancedSettings()
{
	// Render advanced settings using extracted component
	AdvancedSettingsRenderer::RenderAdvancedSettings(
		[this]() { globals::truePBR->DrawSettings(); },
		[this]() { DrawDisableAtBootSettings(); });
}

void Menu::DrawDisableAtBootSettings()
{
	auto state = globals::state;
	auto& disabledFeatures = state->GetDisabledFeatures();

	ImGui::Text(
		"Select features to disable at boot. "
		"This is the same as deleting a feature.ini file. "
		"Restart will be required to reenable.");

	ImGui::Spacing();

	if (ImGui::CollapsingHeader("Special Features", ImGuiTreeNodeFlags_DefaultOpen)) {
		// Prepare a sorted list of special feature names
		std::vector<std::string> specialFeatureNames;
		for (const auto& [featureName, _] : state->specialFeatures) {
			specialFeatureNames.push_back(featureName);
		}
		std::sort(specialFeatureNames.begin(), specialFeatureNames.end());

		// Display sorted special features
		for (const auto& featureName : specialFeatureNames) {
			// Check if the feature is currently disabled
			bool isDisabled = disabledFeatures.contains(featureName) && disabledFeatures[featureName];

			// Create a checkbox for each feature
			if (ImGui::Checkbox(featureName.c_str(), &isDisabled)) {
				// Update the disabledFeatures map based on user interaction
				disabledFeatures[featureName] = isDisabled;
			}
		}
	}

	if (ImGui::CollapsingHeader("Features", ImGuiTreeNodeFlags_DefaultOpen)) {
		// Prepare a sorted list of feature pointers
		auto featureList = Feature::GetFeatureList();
		std::sort(featureList.begin(), featureList.end(), [](Feature* a, Feature* b) {
			return a->GetShortName() < b->GetShortName();
		});

		// Display sorted features
		for (auto* feature : featureList) {
			const std::string featureName = feature->GetShortName();
			bool isDisabled = disabledFeatures.contains(featureName) && disabledFeatures[featureName];

			if (ImGui::Checkbox(featureName.c_str(), &isDisabled)) {
				// Update the disabledFeatures map based on user interaction
				disabledFeatures[featureName] = isDisabled;
			}
		}
	}
}

void Menu::DrawFooter()
{
	ImGui::BulletText(std::format("Game Version: {} {}", magic_enum::enum_name(REL::Module::GetRuntime()), Util::GetFormattedVersion(REL::Module::get().version()).c_str()).c_str());
	ImGui::SameLine();
	ImGui::BulletText(std::format("D3D12 Swap Chain: {}", globals::features::upscaling.d3d12SwapChainActive ? "Active" : "Inactive").c_str());
	ImGui::SameLine();
	ImGui::BulletText(std::format("GPU: {}", globals::state->adapterDescription.c_str()).c_str());
}

/**
 * @brief Main overlay rendering coordinator
 *
 * Delegates all overlay rendering to OverlayRenderer while providing necessary
 * callbacks for input processing, settings rendering, and key mapping. This method
 * serves as the bridge between Menu's state and the extracted overlay rendering logic.
 *
 * Handles VR setup, input event processing, shader compilation status, feature overlays,
 * A/B testing, and ImGui frame management through the specialized renderer component.
 */
void Menu::DrawOverlay()
{
	// Only process reloads when ImGui is NOT in an active frame
	ImGuiContext* ctx = ImGui::GetCurrentContext();
	bool canReload = ctx && !ctx->WithinFrameScope && ctx->WithinEndChildID == 0;

	// Process deferred font reload BEFORE any ImGui operations
	// This is the safest place to do font atlas modifications
	if (pendingFontReload && canReload) {
		// Call ReloadFont first - only clear flag if it succeeds
		if (ThemeManager::ReloadFont(*this, cachedFontSize)) {
			// Reload completed successfully
			pendingFontReload = false;
		} else {
			// Reload failed - keep flag true to retry next frame
			logger::warn("Menu::DrawOverlay() - Font reload failed, will retry next frame");
		}
	}

	// Process deferred icon reload BEFORE rendering
	if (pendingIconReload && canReload) {
		if (Util::IconLoader::InitializeMenuIcons(this)) {
			pendingIconReload = false;
		} else {
			logger::warn("Menu::DrawOverlay() - Icon reload failed, will retry next frame");
		}
	}

	OverlayRenderer::RenderOverlay(
		*this,
		[this]() { ProcessInputEventQueue(); },
		[this]() { DrawSettings(); },
		[](std::vector<InputCombo> keys) -> const char* {
			static std::string result_cache;
			result_cache = Util::Input::KeyIdToString(keys);
			return result_cache.c_str();
		},
		cachedFontSize,
		ThemeManager::ResolveFontSize(*this));
}

/**
 * @brief Processes queued input events for both VR and non-VR devices
 *
 * This method handles the complex logic of routing input events to appropriate handlers:
 * - VR controller events are forwarded to the VR system for specialized processing
 * - Non-VR events (keyboard, mouse) are processed directly for ImGui integration
 * - Includes key state normalization and stuck key detection/correction
 *
 * The method maintains thread safety through mutex protection of the input event queue.
 *
 * @note This method contains Menu-specific logic and state management that makes it
 *       inappropriate for extraction to a utility class.
 */
void Menu::ProcessInputEventQueue()
{
	std::unique_lock<std::shared_mutex> mutex(_inputEventMutex);
	ImGuiIO& io = ImGui::GetIO();
	// Split the queue into VR and non-VR events
	std::vector<KeyEvent> vrEvents;
	std::vector<KeyEvent> nonVREvents;
	for (auto& event : _keyEventQueue) {
		bool isVRController = ((event.device == RE::INPUT_DEVICE::kVivePrimary || event.device == RE::INPUT_DEVICE::kViveSecondary ||
								event.device == RE::INPUT_DEVICE::kOculusPrimary || event.device == RE::INPUT_DEVICE::kOculusSecondary ||
								event.device == RE::INPUT_DEVICE::kWMRPrimary || event.device == RE::INPUT_DEVICE::kWMRSecondary));

		if (globals::features::vr.IsOpenVRCompatible() && isVRController) {
			vrEvents.push_back(event);
		} else {
			nonVREvents.push_back(event);
		}
	}
	// Process VR events in VR
	if (!vrEvents.empty()) {
		globals::features::vr.ProcessVREvents(vrEvents);
		globals::features::vr.UpdateOverlayMenuStateFromInput();
	}

	// Process non-VR events in Menu
	for (auto& event : nonVREvents) {
		if (event.eventType == RE::INPUT_EVENT_TYPE::kChar) {
			io.AddInputCharacter(event.keyCode);
			continue;
		}
		if (event.device == RE::INPUT_DEVICE::kMouse) {
			logger::trace("Detect mouse scan code {} value {} pressed: {}", event.keyCode, event.value, event.IsPressed());
			auto* ew = EditorWindow::GetSingleton();
			bool flying = ew && ew->IsPreviewFlying();
			if (event.keyCode > 7) {  // middle scroll
				if (ew && ew->previewMode == EditorWindow::PreviewMode::FreeCamera) {
					ew->AdjustFlySpeed(event.keyCode == 8 ? 1.0f : -1.0f);
				} else if (!flying) {
					io.AddMouseWheelEvent(0, event.value * (event.keyCode == 8 ? 1 : -1));
				}
			} else if (!flying) {
				if (event.keyCode > 5)
					event.keyCode = 5;
				io.AddMouseButtonEvent(event.keyCode, event.IsPressed());
			}
		}

		if (event.device == RE::INPUT_DEVICE::kKeyboard) {
			uint32_t key = Util::Input::DIKToVK(event.keyCode);
			logger::trace("Detected key code {} ({})", event.keyCode, key);
			if (key == event.keyCode)
				key = MapVirtualKeyEx(event.keyCode, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));

			const bool wasCapturingHotkey = IsCapturingHotkeyInput();
			const bool allowSetupCloseKey = wasCapturingHotkey && HomePageRenderer::ShouldShowFirstTimeSetup() &&
			                                (key == VK_RETURN || key == VK_ESCAPE);
			if (!event.IsPressed()) {
				// Skip key release if it was used to close the first-time setup dialog
				if (HomePageRenderer::ShouldSkipKeyRelease(key)) {
					io.AddKeyEvent(Util::Input::VirtualKeyToImGuiKey(key), event.IsPressed());
					continue;
				}

				struct HotkeyAction
				{
					std::vector<InputCombo>* settingKey;
					bool* settingFlag;
					std::function<void(std::vector<InputCombo>)> action;
				};
				auto shaderCache = globals::shaderCache;
				HotkeyAction hotkeyActions[] = {
					{ &settings.ToggleKey, &settingToggleKey, [this](std::vector<InputCombo> keys) { settings.ToggleKey = keys; settingToggleKey = false; } },
					{ &settings.SkipCompilationKey, &settingSkipCompilationKey, [this](std::vector<InputCombo> keys) { settings.SkipCompilationKey = keys; settingSkipCompilationKey = false; } },
					{ &settings.EffectToggleKey, &settingsEffectsToggle, [this](std::vector<InputCombo> keys) { settings.EffectToggleKey = keys; settingsEffectsToggle = false; } },
					{ &settings.OverlayToggleKey, &settingOverlayToggleKey, [this](std::vector<InputCombo> keys) { settings.OverlayToggleKey = keys; settingOverlayToggleKey = false; } },
					{ &settings.ShaderBlockPrevKey, &settingShaderBlockPrevKey, [this](std::vector<InputCombo> keys) { settings.ShaderBlockPrevKey = keys; settingShaderBlockPrevKey = false; } },
					{ &settings.ShaderBlockNextKey, &settingShaderBlockNextKey, [this](std::vector<InputCombo> keys) { settings.ShaderBlockNextKey = keys; settingShaderBlockNextKey = false; } },
					{ &settings.WeatherEditorToggleKey, &settingWeatherEditorToggleKey, [this](std::vector<InputCombo> keys) { settings.WeatherEditorToggleKey = keys; settingWeatherEditorToggleKey = false; } },
				};
				bool handled = false;
				for (auto& h : hotkeyActions) {
					if (*(h.settingFlag)) {
						// During first-time setup, don't capture Enter or Escape as hotkeys
						// These keys are reserved for closing the dialog, unless we are recording a modifier
						if (HomePageRenderer::ShouldShowFirstTimeSetup() && (key == VK_RETURN || key == VK_ESCAPE)) {
							// Do not stop capture here, just let it pass through to the UI
							// The UI code in HomePageRenderer checks for Enter/Escape and completes setup
							*(h.settingFlag) = false;  // Cancel hotkey capture mode
							handled = true;
							break;
						}

						// Ignore modifier-only key releases during recording
						bool isModifier = (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
										   key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
										   key == VK_MENU || key == VK_LMENU || key == VK_RMENU);

						if (isModifier) {
							handled = true;
							break;
						}

						// Capture modifiers + key
						std::vector<InputCombo> combo;

						// Add active modifiers to combo
						if ((GetAsyncKeyState(VK_CONTROL) & Constants::KEY_PRESSED_MASK) &&
							key != VK_CONTROL && key != VK_LCONTROL && key != VK_RCONTROL)
							combo.push_back(InputCombo::Keyboard(VK_CONTROL));
						if ((GetAsyncKeyState(VK_SHIFT) & Constants::KEY_PRESSED_MASK) &&
							key != VK_SHIFT && key != VK_LSHIFT && key != VK_RSHIFT)
							combo.push_back(InputCombo::Keyboard(VK_SHIFT));
						if ((GetAsyncKeyState(VK_MENU) & Constants::KEY_PRESSED_MASK) &&
							key != VK_MENU && key != VK_LMENU && key != VK_RMENU)
							combo.push_back(InputCombo::Keyboard(VK_MENU));

						combo.push_back(InputCombo::Keyboard(key));

						h.action(combo);
						handled = true;
						break;
					}
				}
				if (!handled) {
					struct KeyAction
					{
						std::vector<InputCombo>& settingKey;
						std::function<void()> action;
					};
					KeyAction keyActions[] = {
						{ settings.ToggleKey, [this]() {
							 if (!HomePageRenderer::ShouldShowFirstTimeSetup()) {
								 IsEnabled = !IsEnabled;
								 if (IsEnabled)
									 ImGui::GetIO().ClearInputKeys();  // Prevent toggle key from remaining "held" in ImGui after open.
							 }
						 } },
						{ settings.SkipCompilationKey, [this, shaderCache]() { if (!ShouldSwallowInput() && shaderCache->IsCompiling()) shaderCache->backgroundCompilation = true; } },
						{ settings.EffectToggleKey, [shaderCache]() { shaderCache->SetEnabled(!shaderCache->IsEnabled()); } },
						{ settings.ShaderBlockPrevKey, [this, shaderCache]() { if (settings.EnableShaderBlocking) shaderCache->IterateShaderBlock(); } },
						{ settings.ShaderBlockNextKey, [this, shaderCache]() { if (settings.EnableShaderBlocking) shaderCache->IterateShaderBlock(false); } },
						{ settings.OverlayToggleKey, []() { Menu::GetSingleton()->overlayVisible = !Menu::GetSingleton()->overlayVisible; } },
						{ settings.WeatherEditorToggleKey, []() {
							 auto* ew = EditorWindow::GetSingleton();
							 if (!ew)
								 return;
							 if (ew->GetPreviewMode() == EditorWindow::PreviewMode::FreeCamera) {
								 // Flying → lock camera position for editing
								 ew->ToggleFreeCameraLock();
							 } else if (ew->IsInPreviewMode()) {
								 // Locked or PlayMode → fully exit preview
								 ew->ExitPreviewMode();
							 } else if (EditorWindow::CanBeOpen()) {
								 ew->open = !ew->open;
							 }
						 } },
					};
					for (const auto& ka : keyActions) {
						if (InputCombo::MatchesKeyboardCombo(ka.settingKey, key)) {
							ka.action();
							break;
						}
					}
				}

				// Handle ESC key for menu and editor window
				auto* editorWindow = EditorWindow::GetSingleton();
				if (key == VK_ESCAPE) {
					if (editorWindow && editorWindow->IsInPreviewMode()) {
						editorWindow->ExitPreviewMode();
					} else if (editorWindow && editorWindow->open && editorWindow->ShouldHandleEscapeKey()) {
						editorWindow->open = false;
					} else if (IsEnabled && (!editorWindow || !editorWindow->open)) {
						IsEnabled = false;
					}
				}
			}

			// Don't forward hotkey events to ImGui when input is captured (prevents e.g. End key scrolling the feature list)
			// SkipCompilationKey (ESC) is excluded — ESC must reach ImGui for menu/dialog close.
			const std::vector<InputCombo>* hotkeys[] = {
				&settings.ToggleKey, &settings.EffectToggleKey,
				&settings.OverlayToggleKey, &settings.ShaderBlockPrevKey, &settings.ShaderBlockNextKey,
				&settings.WeatherEditorToggleKey
			};
			bool isHotkey = ShouldSwallowInput() && std::any_of(std::begin(hotkeys), std::end(hotkeys),
														[key](const auto* combo) { return InputCombo::MatchesKeyboardCombo(*combo, key); });

			// Always forward key-up events. Suppress key-down during active hotkeys,
			// and during hotkey capture except setup close keys (Enter/Escape).
			const bool isKeyDown = event.IsPressed();
			const bool suppressForwarding = isKeyDown && (isHotkey || (wasCapturingHotkey && !allowSetupCloseKey));
			if (!suppressForwarding) {
				// DirectInput loses key-up events after alt-tab; validate against OS state.
				bool pressed = isKeyDown && (GetAsyncKeyState(key) & Constants::KEY_PRESSED_MASK);
				io.AddKeyEvent(Util::Input::VirtualKeyToImGuiKey(key), pressed);

				if (key == VK_LCONTROL || key == VK_RCONTROL)
					io.AddKeyEvent(ImGuiMod_Ctrl, pressed);
				else if (key == VK_LSHIFT || key == VK_RSHIFT)
					io.AddKeyEvent(ImGuiMod_Shift, pressed);
				else if (key == VK_LMENU || key == VK_RMENU)
					io.AddKeyEvent(ImGuiMod_Alt, pressed);
			}
		}

		if (event.device == RE::INPUT_DEVICE::kGamepad) {
			if (event.eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
				// Map thumbstick axes to ImGui analog stick keys
				constexpr float kDeadzone = 0.15f;
				if (event.keyCode == 0x0B) {  // Left stick
					io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, event.thumbstickX < -kDeadzone, event.thumbstickX < -kDeadzone ? -event.thumbstickX : 0.0f);
					io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, event.thumbstickX > kDeadzone, event.thumbstickX > kDeadzone ? event.thumbstickX : 0.0f);
					io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, event.thumbstickY > kDeadzone, event.thumbstickY > kDeadzone ? event.thumbstickY : 0.0f);
					io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, event.thumbstickY < -kDeadzone, event.thumbstickY < -kDeadzone ? -event.thumbstickY : 0.0f);
				} else if (event.keyCode == 0x0C) {  // Right stick
					io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickLeft, event.thumbstickX < -kDeadzone, event.thumbstickX < -kDeadzone ? -event.thumbstickX : 0.0f);
					io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickRight, event.thumbstickX > kDeadzone, event.thumbstickX > kDeadzone ? event.thumbstickX : 0.0f);
					io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickUp, event.thumbstickY > kDeadzone, event.thumbstickY > kDeadzone ? event.thumbstickY : 0.0f);
					io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickDown, event.thumbstickY < -kDeadzone, event.thumbstickY < -kDeadzone ? -event.thumbstickY : 0.0f);
				}
			} else {
				// Map XInput button masks to ImGui gamepad keys
				ImGuiKey imKey = ImGuiKey_None;
				switch (event.keyCode) {
				case 0x1000: imKey = ImGuiKey_GamepadFaceDown; break;   // A
				case 0x2000: imKey = ImGuiKey_GamepadFaceRight; break;  // B
				case 0x4000: imKey = ImGuiKey_GamepadFaceLeft; break;   // X
				case 0x8000: imKey = ImGuiKey_GamepadFaceUp; break;     // Y
				case 0x0001: imKey = ImGuiKey_GamepadDpadUp; break;
				case 0x0002: imKey = ImGuiKey_GamepadDpadDown; break;
				case 0x0004: imKey = ImGuiKey_GamepadDpadLeft; break;
				case 0x0008: imKey = ImGuiKey_GamepadDpadRight; break;
				case 0x0010: imKey = ImGuiKey_GamepadStart; break;
				case 0x0020: imKey = ImGuiKey_GamepadBack; break;
				case 0x0100: imKey = ImGuiKey_GamepadL1; break;  // LB
				case 0x0200: imKey = ImGuiKey_GamepadR1; break;  // RB
				case 0x0040: imKey = ImGuiKey_GamepadL3; break;  // LS click
				case 0x0080: imKey = ImGuiKey_GamepadR3; break;  // RS click
				case 0x0009: imKey = ImGuiKey_GamepadL2; break;  // LT
				case 0x000A: imKey = ImGuiKey_GamepadR2; break;  // RT
				}
				if (imKey != ImGuiKey_None) {
					if (imKey == ImGuiKey_GamepadL2 || imKey == ImGuiKey_GamepadR2)
						io.AddKeyAnalogEvent(imKey, event.IsPressed(), event.IsPressed() ? event.value : 0.0f);
					else
						io.AddKeyEvent(imKey, event.IsPressed());
				}
			}
		}
	}

	_keyEventQueue.clear();
}

bool Menu::IsCapturingHotkeyInput() const
{
	return settingToggleKey || settingSkipCompilationKey || settingsEffectsToggle ||
	       settingOverlayToggleKey || settingShaderBlockPrevKey || settingShaderBlockNextKey || settingWeatherEditorToggleKey;
}

void Menu::addToEventQueue(KeyEvent e)
{
	std::unique_lock<std::shared_mutex> mutex(_inputEventMutex);
	_keyEventQueue.emplace_back(e);
}

void Menu::OnFocusChanged()
{
	// Solves the alt+tab stuck issue, but disables tab after tabbing back in.
	if (const auto& inputMgr = RE::BSInputDeviceManager::GetSingleton()) {
		if (const auto& device = inputMgr->GetKeyboard()) {
			device->ClearInputState();
		}
	}
	// Allows tab to work again after alt+tabbing back in.
	ImGui::GetIO().ClearInputKeys();
}

void Menu::ProcessInputEvents(RE::InputEvent* const* a_events)
{
	for (auto it = *a_events; it; it = it->next) {
		// Accept button, char, and thumbstick events
		if (it->GetEventType() != RE::INPUT_EVENT_TYPE::kButton &&
			it->GetEventType() != RE::INPUT_EVENT_TYPE::kChar &&

			it->GetEventType() != RE::INPUT_EVENT_TYPE::kThumbstick

			)  // we do not care about non button/char/thumbstick events
			continue;

		if (it->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
			addToEventQueue(KeyEvent(static_cast<RE::ButtonEvent*>(it)));
		} else if (it->GetEventType() == RE::INPUT_EVENT_TYPE::kChar) {
			addToEventQueue(KeyEvent(static_cast<CharEvent*>(it)));

		} else if (it->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick) {
			addToEventQueue(KeyEvent(static_cast<RE::ThumbstickEvent*>(it)));
		}
	}
}

bool Menu::ShouldSwallowInput()
{
	auto editorWindow = EditorWindow::GetSingleton();
	return IsEnabled || HomePageRenderer::ShouldShowFirstTimeSetup() || (editorWindow && editorWindow->open);
}

bool Menu::IsPreviewFlying()
{
	auto editorWindow = EditorWindow::GetSingleton();
	return editorWindow && editorWindow->IsPreviewFlying();
}

void Menu::SelectFeatureMenu(const std::string& featureName)
{
	pendingFeatureSelection = featureName;
	logger::info("Queued navigation to {} feature menu", featureName);
}

/**
 * @brief Renders the standalone weather details window when enabled
 *
 * Delegates to the WeatherEditor feature for rendering the weather details window
 * that can remain open even when the main menu is closed. This provides a simple
 * coordination layer between the Menu system and the WeatherEditor feature.
 */
void Menu::DrawWeatherDetailsWindow()
{
	if (!globals::features::weatherEditor.WeatherDetailsWindow.Enabled) {
		return;
	}
	if (!globals::features::weatherEditor.loaded) {
		return;
	}

	// Use Weather core feature for all window management and rendering
	auto& weather = globals::features::weatherEditor;
	bool* p_open = &globals::features::weatherEditor.WeatherDetailsWindow.Enabled;
	weather.RenderWeatherDetailsWindow(p_open);
}

/**
 * @brief Builds category counts for feature organization and display
 *
 * Iterates through all loaded features and counts how many features belong to each
 * category. This information is used for UI organization and displaying category
 * statistics in the feature navigation interface.
 *
 * @note Only counts features that are both loaded and configured to appear in the menu.
 */
void Menu::BuildCategoryCounts()
{
	const std::vector<Feature*>& features = Feature::GetFeatureList();
	// Get the category of each feature, and increment the count for that category
	for (auto& feature : features) {
		if (feature->IsInMenu() && feature->loaded) {
			std::string_view category = feature->GetCategory();
			categoryCounts[std::string(category)]++;
		}
	}
}

// =====================================================================
// BasicMenu — transparent in-game panel injected into the journal menu.
// =====================================================================

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

	// Window flags — no title bar for a cleaner, game-like panel
	constexpr ImGuiWindowFlags kBasicMenuWindowFlags =
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoTitleBar;
}

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

// --- Feature list building ---

// Features excluded from the basic menu — advanced/niche features
// that belong in the full settings menu only.
namespace
{
	const std::unordered_set<std::string> kExcludedFeatures = {
		"InverseSquareLighting",
		"LinearLighting",
		"ImageBasedLighting",
		"ExponentialHeightFog",
		"Skylighting",
		"CloudShadows",
	};

	bool IsExcludedCategory(std::string_view a_cat)
	{
		return a_cat == FeatureCategories::kUtility || a_cat == FeatureCategories::kOther;
	}

	// Curated one-line descriptions, performance badge, and intensity level
	// perfLevel: 0=light(green), 1=medium(yellow), 2=heavy(red)
	struct FeatureMeta
	{
		const char* description;
		const char* badge;  // "[GPU]", "[VRAM/GPU]", etc.
		int perfLevel;
	};
	const std::unordered_map<std::string, FeatureMeta> kFeatureMeta = {
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
}

void BasicMenu::RebuildFeatureList()
{
	featureList.clear();

	for (auto* feat : Feature::GetFeatureList()) {
		if (!feat->IsInMenu())
			continue;

		const auto shortName = feat->GetShortName();

		// Include if currently loaded OR if user toggled it off this session via the
		// boot-disable fallback (so it stays visible in the list for re-enabling).
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
namespace
{
	constexpr float kBgAlpha = 0.68f;
	constexpr float kBorderSize = 4.0f;

	constexpr ImVec4 kColorBg{ 0.0f, 0.0f, 0.0f, kBgAlpha };
	constexpr ImVec4 kColorBorder{ 0.25f, 0.25f, 0.25f, 1.0f };
	constexpr ImVec4 kColorText{ 0.733f, 0.741f, 0.749f, 1.0f };
	constexpr ImVec4 kColorTextDim{ 0.733f, 0.741f, 0.749f, 0.30f };
	constexpr ImVec4 kColorFrameBg{ 1.0f, 1.0f, 1.0f, 0.063f };
	constexpr ImVec4 kColorFrameBgHovered{ 1.0f, 1.0f, 1.0f, 0.2f };
	constexpr ImVec4 kColorButton{ 0.0f, 0.0f, 0.0f, 1.0f };
	constexpr ImVec4 kColorButtonHovered{ 1.0f, 1.0f, 1.0f, 0.2f };
	constexpr ImVec4 kColorButtonActive{ 0.60f, 0.60f, 0.60f, 1.0f };
	constexpr ImVec4 kColorNavHighlight{ 1.0f, 1.0f, 1.0f, 1.0f };
	constexpr ImVec4 kColorSeparator{ 0.25f, 0.25f, 0.25f, 1.0f };
	constexpr ImVec4 kColorScrollbarBg{ 0.0f, 0.0f, 0.0f, 0.0f };
	constexpr ImVec4 kColorScrollbarGrab{ 0.31f, 0.31f, 0.31f, 1.0f };

	// Toggle switch colors (ported from FUCK)
	constexpr ImVec4 kColorToggleRail{ 0.0f, 0.0f, 0.0f, 1.0f };
	constexpr ImVec4 kColorToggleBorder{ 0.25f, 0.25f, 0.25f, 1.0f };
	constexpr ImVec4 kColorToggleBorderActive{ 0.60f, 0.60f, 0.60f, 1.0f };
	constexpr ImVec4 kColorToggleKnob{ 0.0f, 0.0f, 0.0f, 1.0f };

	// Performance badge colors
	constexpr ImVec4 kColorPerfHeavy{ 1.0f, 0.35f, 0.35f, 1.0f };
	constexpr ImVec4 kColorPerfMedium{ 1.0f, 0.8f, 0.3f, 1.0f };
	constexpr ImVec4 kColorPerfLight{ 0.5f, 0.9f, 0.5f, 1.0f };

	// VRAM bar colors
	constexpr ImVec4 kColorVRAMOk{ 0.3f, 0.8f, 0.3f, 1.0f };
	constexpr ImVec4 kColorVRAMWarn{ 1.0f, 0.8f, 0.2f, 1.0f };
	constexpr ImVec4 kColorVRAMCrit{ 1.0f, 0.3f, 0.3f, 1.0f };

	constexpr int kStyleColorCount = 15;
	constexpr int kStyleVarCount = 3;

	void PushPanelStyle()
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

	void PopPanelStyle()
	{
		ImGui::PopStyleVar(kStyleVarCount);
		ImGui::PopStyleColor(kStyleColorCount);
	}
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

	if (!ImGui::Begin("##CSBasicMenu", nullptr, kBasicMenuWindowFlags)) {
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
	// Quality preset bar — applies QualityLevel::Low/Medium/High to every loaded feature
	// that exposes presets via GetQualityPresets(). Features without presets are unaffected.
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
				const auto level = static_cast<Feature::QualityLevel>(i);
				for (auto* feat : Feature::GetFeatureList()) {
					if (feat->loaded)
						feat->ApplyQualityPreset(level);
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

	// Prefer the feature's runtime Enabled flag when exposed; fall back to the boot-disable
	// flag (`feature->loaded`) for features that don't expose one. The boot-disable path
	// also tracks userToggledOff so the entry stays visible after toggling off.
	bool* runtimeFlag = a_entry.feature->GetEnabledFlag();
	bool enabled = runtimeFlag ? *runtimeFlag : a_entry.feature->loaded;

	if (DrawToggle(a_entry.feature->GetName().c_str(), &enabled)) {
		if (runtimeFlag) {
			*runtimeFlag = enabled;
		} else {
			a_entry.feature->loaded = enabled;
			if (!enabled)
				userToggledOff.insert(a_entry.feature->GetShortName());
			else
				userToggledOff.erase(a_entry.feature->GetShortName());
		}
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
	bool* runtimeFlag = a_entry.feature->GetEnabledFlag();
	bool enabled = runtimeFlag ? *runtimeFlag : a_entry.feature->loaded;

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
		if (runtimeFlag) {
			*runtimeFlag = enabled;
		} else {
			a_entry.feature->loaded = enabled;
			if (!enabled)
				userToggledOff.insert(a_entry.feature->GetShortName());
			else
				userToggledOff.erase(a_entry.feature->GetShortName());
		}
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
