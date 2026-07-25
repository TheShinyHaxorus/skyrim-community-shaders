#include "HomePageRenderer.h"
#include "PCH.h"

#include <imgui.h>

#include "FeatureConstraints.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Plugin.h"
#include "State.h"
#include "ThemeManager.h"

// Static member definitions
bool HomePageRenderer::isFirstTimeSetupShown = false;
uint32_t HomePageRenderer::keyThatClosedDialog = 0;

bool HomePageRenderer::ShouldSkipKeyRelease(uint32_t key)
{
	if (keyThatClosedDialog && key == keyThatClosedDialog) {
		keyThatClosedDialog = 0;
		return true;
	}
	return false;
}

void HomePageRenderer::RenderHomePage()
{
	ImGui::BeginChild("HomePage", ImVec2(0, 0), false);

	RenderMastheadSection();

	RenderActiveConstraintsSection();

	RenderQuickLinksSection();

	RenderFAQSection();

	ImGui::EndChild();
}

void HomePageRenderer::DrawOrnamentalRule(float widthRatio)
{
	const float scale = Util::GetUIScale();
	const float avail = ImGui::GetContentRegionAvail().x;
	if (avail <= 0.0f) {
		return;
	}

	const float diamond = RULE_DIAMOND_RADIUS * scale;
	const float padding = RULE_VERTICAL_PADDING * scale;

	ImGui::Dummy(ImVec2(0.0f, padding));

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float centerX = origin.x + avail * 0.5f;
	const float centerY = origin.y + diamond;
	const float halfWidth = std::max(RULE_MIN_WIDTH * scale, avail * widthRatio) * 0.5f;
	const float gap = diamond * RULE_DIAMOND_GAP;
	const float thickness = std::max(1.0f, RULE_THICKNESS * scale);

	ImVec4 sepColor = globals::menu ? globals::menu->GetTheme().Palette.Separator : ImVec4(0.5f, 0.5f, 0.5f, 0.6f);
	const ImU32 solid = ImGui::GetColorU32(sepColor);
	sepColor.w = 0.0f;
	const ImU32 fade = ImGui::GetColorU32(sepColor);

	ImDrawList* drawList = ImGui::GetWindowDrawList();

	// Each arm fades out toward the outer edge, mirroring Skyrim's tapered rules.
	drawList->AddRectFilledMultiColor(
		ImVec2(centerX - halfWidth, centerY - thickness * 0.5f),
		ImVec2(centerX - gap, centerY + thickness * 0.5f),
		fade, solid, solid, fade);
	drawList->AddRectFilledMultiColor(
		ImVec2(centerX + gap, centerY - thickness * 0.5f),
		ImVec2(centerX + halfWidth, centerY + thickness * 0.5f),
		solid, fade, fade, solid);

	drawList->AddQuadFilled(
		ImVec2(centerX, centerY - diamond),
		ImVec2(centerX + diamond, centerY),
		ImVec2(centerX, centerY + diamond),
		ImVec2(centerX - diamond, centerY),
		solid);

	ImGui::Dummy(ImVec2(0.0f, diamond * 2.0f + padding));
}

void HomePageRenderer::DrawSectionTitle(const char* title)
{
	MenuFonts::FontRoleGuard headingFont(Menu::FontRole::Heading);

	const float avail = ImGui::GetContentRegionAvail().x;
	const float offset = (avail - ImGui::CalcTextSize(title).x) * 0.5f;
	if (offset > 0.0f) {
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
	}
	ImGui::TextUnformatted(title);

	DrawOrnamentalRule();
}

void HomePageRenderer::DrawFAQGroupLabel(const char* label)
{
	ImGui::Dummy(ImVec2(0.0f, FAQ_GROUP_TOP_SPACING * Util::GetUIScale()));

	MenuFonts::FontRoleGuard subtextFont(Menu::FontRole::Subtext);

	ImVec4 color = globals::menu ? globals::menu->GetTheme().Palette.Text : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	color.w *= FAQ_GROUP_LABEL_ALPHA;
	ImGui::TextColored(color, "%s", label);
}

void HomePageRenderer::DrawFAQEntry(const char* question, const char* answer)
{
	if (ImGui::CollapsingHeader(question)) {
		ImGui::Indent(FAQ_ANSWER_INDENT * Util::GetUIScale());
		ImGui::TextWrapped("%s", answer);
		ImGui::Spacing();
		ImGui::Unindent(FAQ_ANSWER_INDENT * Util::GetUIScale());
	}
}

void HomePageRenderer::RenderMastheadSection()
{
	const float scale = Util::GetUIScale();
	auto* menu = Menu::GetSingleton();

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 6 * scale));

	// Emblem, centred above the wordmark
	if (menu && menu->uiIcons.logo.texture && menu->uiIcons.logo.size.y > 0.0f) {
		const float emblemHeight = MASTHEAD_EMBLEM_HEIGHT * scale;
		const float emblemWidth = emblemHeight * (menu->uiIcons.logo.size.x / menu->uiIcons.logo.size.y);

		const float offset = (ImGui::GetContentRegionAvail().x - emblemWidth) * 0.5f;
		if (offset > 0.0f) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
		}

		ImVec4 tint(1.0f, 1.0f, 1.0f, MASTHEAD_EMBLEM_ALPHA);
		if (menu->GetSettings().Theme.UseMonochromeLogo) {
			tint = menu->GetSettings().Theme.Palette.Text;
			tint.w *= MASTHEAD_EMBLEM_ALPHA;
		}
		ImGui::ImageWithBg(menu->uiIcons.logo.texture, ImVec2(emblemWidth, emblemHeight), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
	}

	// Wordmark with version
	auto versionStr = Util::GetFormattedVersion(Plugin::VERSION);
	auto expectedTag = std::format("v{}", versionStr);
	auto* i18n = I18n::GetSingleton();
	std::string titleWithVersion = Plugin::BUILD_DESCRIBE == expectedTag ?
	                                   i18n->Format("menu.home.welcome", { { "version", versionStr } }, "Bottled Shaders {version}") :
	                                   i18n->Format("menu.home.welcome_dev", { { "version", versionStr }, { "build", std::string(Plugin::BUILD_DESCRIBE) } }, "Bottled Shaders {version} [{build}]");

	{
		MenuFonts::FontRoleGuard titleFont(Menu::FontRole::Title);
		ImGui::SetWindowFontScale(TITLE_FONT_SCALE);

		const float offset = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(titleWithVersion.c_str()).x) * 0.5f;
		if (offset > 0.0f) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
		}
		ImGui::TextUnformatted(titleWithVersion.c_str());

		ImGui::SetWindowFontScale(1.0f);
	}

	// Single-line tagline, dimmed so the wordmark stays dominant
	{
		MenuFonts::FontRoleGuard subtextFont(Menu::FontRole::Subtext);

		const char* introText = T("menu.home.intro", "Modern rendering for Skyrim - enable only what you want.");
		const float offset = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(introText).x) * 0.5f;
		if (offset > 0.0f) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
		}

		ImVec4 color = globals::menu ? globals::menu->GetTheme().Palette.Text : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		color.w *= SUBTITLE_ALPHA;
		ImGui::TextColored(color, "%s", introText);
	}

	ImGui::PopStyleVar();

	DrawOrnamentalRule();
}

void HomePageRenderer::RenderQuickLinksSection()
{
	DrawSectionTitle(T("menu.home.quick_links", "Quick Links"));

	ImGui::Columns(4, nullptr, false);

	if (ImGui::Button(T("menu.home.nexus_mods", "Nexus Mods"), ImVec2(-1, 0))) {
		ShellExecuteA(NULL, "open", "https://www.nexusmods.com/skyrimspecialedition/mods/86492", NULL, NULL, SW_SHOWNORMAL);
	}

	ImGui::NextColumn();
	if (ImGui::Button(T("menu.home.github", "Source Code"), ImVec2(-1, 0))) {
		ShellExecuteA(NULL, "open", "https://github.com/doodlum/skyrim-community-shaders", NULL, NULL, SW_SHOWNORMAL);
	}

	ImGui::NextColumn();
	if (ImGui::Button(T("menu.home.wiki", "User Guide"), ImVec2(-1, 0))) {
		ShellExecuteA(NULL, "open", "https://modding.wiki/en/skyrim/developers/community-shaders", NULL, NULL, SW_SHOWNORMAL);
	}

	ImGui::NextColumn();
	if (ImGui::Button(T("menu.home.dev_wiki", "Developer Docs"), ImVec2(-1, 0))) {
		ShellExecuteA(NULL, "open", "https://github.com/doodlum/skyrim-community-shaders/wiki", NULL, NULL, SW_SHOWNORMAL);
	}

	ImGui::Columns(1);
}

void HomePageRenderer::RenderFAQSection()
{
	DrawSectionTitle(T("menu.faq.title", "Frequently Asked Questions"));

	DrawFAQGroupLabel(T("menu.faq.group_getting_started", "GETTING STARTED"));

	DrawFAQEntry(
		T("menu.faq.q1", "What is Bottled Shaders?"),
		T("menu.faq.a1",
			"A modular graphics framework for Skyrim. It replaces the game's shaders at runtime to add "
			"modern lighting, materials, and post-processing. Every feature is independent, so you can "
			"enable only the ones you want."));

	DrawFAQEntry(
		T("menu.faq.q2", "How do I configure a feature?"),
		T("menu.faq.a2",
			"Pick a feature from the sidebar to open its settings. Most features ship with presets to start "
			"from, and every setting has a tooltip explaining what it does and what it costs."));

	DrawFAQEntry(
		T("menu.faq.q7", "How do I change the menu hotkey?"),
		T("menu.faq.a7",
			"The menu opens with END by default. Rebind it under General > Keybindings, or edit the hotkey "
			"directly in the JSON configuration files."));

	DrawFAQGroupLabel(T("menu.faq.group_performance", "PERFORMANCE"));

	DrawFAQEntry(
		T("menu.faq.q5", "How do I improve performance?"),
		T("menu.faq.a5",
			"Turn on the Performance Overlay first so you can see which passes cost the most. From there: lower "
			"the quality of expensive features such as Screen Space GI, or turn them off entirely; enable "
			"upscaling under the Display tab. Features you never use are best set to \"Disable at Boot\" so they "
			"cost nothing at all."));

	DrawFAQGroupLabel(T("menu.faq.group_troubleshooting", "TROUBLESHOOTING"));

	DrawFAQEntry(
		T("menu.faq.q3", "A feature won't load, or shaders failed to compile"),
		T("menu.faq.a3",
			"Both usually mean mismatched files. Check the Feature Issues tab first - it names the specific "
			"feature and problem. The common causes are feature files left over from an older version, files "
			"mixed in from a test build, or a feature whose functionality is now built in and should be "
			"uninstalled. Update every feature to the matching version and remove obsolete ones, then clear the "
			"shader cache."));

	DrawFAQEntry(
		T("menu.faq.q6", "Is it compatible with ENB?"),
		T("menu.faq.a6",
			"No. If ENB is detected at startup, all hooks and features disable themselves automatically to avoid "
			"conflicts. Use one or the other, not both."));

	DrawFAQGroupLabel(T("menu.faq.group_about", "ABOUT"));

	DrawFAQEntry(
		T("menu.faq.q9", "Is it open source? Can I contribute?"),
		T("menu.faq.a9",
			"Yes. The source is on GitHub under the GPL, and contributions are welcome - shader work, C++, and "
			"documentation alike. See the developer documentation for build instructions and contribution "
			"guidelines.\n\n"
			"Note that branding assets (logos, icons, and typography) are not covered by the GPL and may not be "
			"reused without permission."));
}

void HomePageRenderer::RenderActiveConstraintsSection()
{
	auto constraints = FeatureConstraints::GetAllActiveConstraints();
	if (constraints.empty()) {
		return;  // Don't show section if there are no active constraints
	}

	ImGui::Spacing();

	// Use warning color for the header to draw attention
	ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
	bool headerOpen = ImGui::CollapsingHeader(T("menu.home.active_constraints", "Active Setting Constraints"), ImGuiTreeNodeFlags_None);
	ImGui::PopStyleColor();

	if (headerOpen) {
		ImGui::TextWrapped("%s", T("menu.home.constraints_desc", "Some settings are constrained by other features. Hover over rows for details."));

		ImGui::Spacing();

		// Prepare data for table
		struct ConstraintRow
		{
			std::string setting;
			std::string forcedTo;
			std::string constrainedBy;
			std::string firstSourceShortName;  // For "navigate to feature" on click
			std::string tooltip;
		};

		std::vector<ConstraintRow> rows;
		for (const auto& [settingId, result] : constraints) {
			ConstraintRow row;
			row.setting = std::format("{}.{}", settingId.featureShortName, settingId.settingPath);
			row.forcedTo = FeatureConstraints::FormatConstraintValue(result.forcedValue);
			for (size_t i = 0; i < result.sources.size(); ++i) {
				if (i > 0)
					row.constrainedBy += ", ";
				row.constrainedBy += result.sources[i].featureName;
			}
			if (!result.sources.empty()) {
				row.firstSourceShortName = result.sources[0].featureShortName;
			}
			// Build tooltip
			for (const auto& src : result.sources) {
				if (!row.tooltip.empty())
					row.tooltip += "\n";
				row.tooltip += std::format("{}: {}", src.featureName, src.reason);
				if (src.recommendDisableAtBoot) {
					row.tooltip += std::string("\n") + T("menu.home.consider_disabling_at_boot", "Consider disabling at boot.");
				}
			}
			rows.push_back(row);
		}

		// Define headers
		std::vector<std::string> headers = { T("menu.home.constraint_header_setting", "Setting"), T("menu.home.constraint_header_forced_to", "Forced To"), T("menu.home.constraint_header_constrained_by", "Constrained By") };

		// Custom sorts (string comparators for each column)
		std::vector<std::function<bool(const ConstraintRow&, const ConstraintRow&, bool)>> customSorts = {
			[](const ConstraintRow& a, const ConstraintRow& b, bool asc) { return Util::StringSortComparator(a.setting, b.setting, asc); },
			[](const ConstraintRow& a, const ConstraintRow& b, bool asc) { return Util::StringSortComparator(a.forcedTo, b.forcedTo, asc); },
			[](const ConstraintRow& a, const ConstraintRow& b, bool asc) { return Util::StringSortComparator(a.constrainedBy, b.constrainedBy, asc); }
		};

		// Cell render -- column 2 ("Constrained By") is clickable to navigate
		// to the first source feature's settings page.
		auto cellRender = [](int rowIdx, int colIdx, const ConstraintRow& row) {
			if (colIdx == 0) {
				Util::RenderTableCell(row.setting, "", "", nullptr, ImVec4(1, 1, 1, 1), true, Util::Colors::GetWarning());
			} else if (colIdx == 1) {
				Util::RenderTableCell(row.forcedTo, "", "", nullptr, ImVec4(1, 1, 1, 1), true);
			} else if (colIdx == 2) {
				if (!row.firstSourceShortName.empty()) {
					if (ImGui::Selectable(std::format("{}##nav{}", row.constrainedBy, rowIdx).c_str())) {
						if (auto* menu = Menu::GetSingleton()) {
							menu->SelectFeatureMenu(row.firstSourceShortName);
						}
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", I18n::GetSingleton()->Format("menu.home.click_to_navigate",
																  { { "feature", row.constrainedBy } },
																  "Click to navigate to {feature}")
											  .c_str());
						if (!row.tooltip.empty()) {
							ImGui::Separator();
							ImGui::Text("%s", row.tooltip.c_str());
						}
					}
				} else {
					Util::RenderTableCell(row.constrainedBy, "", row.tooltip, nullptr, ImVec4(1, 1, 1, 1), true);
				}
			}
		};

		// Render table
		Util::ShowSortedStringTableCustom<ConstraintRow>(
			"ConstraintsTable",
			headers,
			rows,
			0,     // sortColumn
			true,  // ascending
			customSorts,
			cellRender);
	}

	ImGui::Spacing();
}

void HomePageRenderer::RenderFirstTimeSetupDialog()
{
	if (!ShouldShowFirstTimeSetup()) {
		return;
	}

	// Block input to the game and make cursor visible - input blocking is handled by ShouldSwallowInput()
	auto& io = ImGui::GetIO();
	io.WantCaptureMouse = true;
	io.WantCaptureKeyboard = true;
	io.MouseDrawCursor = true;  // Show ImGui cursor

	float uiScale = Util::GetUIScale();
	ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(DIALOG_MIN_WIDTH * uiScale, 0), ImVec2(DIALOG_MAX_WIDTH * uiScale, FLT_MAX));
	ImGui::SetNextWindowFocus();

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, DIALOG_CORNER_ROUNDING * uiScale);

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
	                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize;

	if (!ImGui::Begin("##FirstTimeSetup", nullptr, flags)) {
		ImGui::PopStyleVar();
		ImGui::End();
		return;
	}

	// Fullscreen fade on the dialog's draw list — covers all windows beneath at the dialog's z-position
	auto* drawList = ImGui::GetWindowDrawList();
	drawList->PushClipRectFullScreen();
	drawList->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, MODAL_OVERLAY_ALPHA));
	drawList->PopClipRect();

	auto menu = Menu::GetSingleton();

	// Render CS logo as background watermark with proper aspect ratio
	if (menu && menu->uiIcons.logo.texture) {
		ImVec2 windowPos = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();

		// Get the original texture size to maintain aspect ratio
		ImVec2 textureSize = menu->uiIcons.logo.size;
		float aspectRatio = textureSize.x / textureSize.y;

		// Set desired height and calculate width to maintain aspect ratio
		float logoHeight = LOGO_WATERMARK_HEIGHT * uiScale;
		float logoWidth = logoHeight * aspectRatio;

		ImVec2 logoMin(windowPos.x + (windowSize.x - logoWidth) * 0.5f,
			windowPos.y + (windowSize.y - logoHeight) * 0.5f);
		ImVec2 logoMax(logoMin.x + logoWidth, logoMin.y + logoHeight);

		// Determine watermark color based on monochrome logo setting
		ImU32 watermarkColor;
		if (menu->GetSettings().Theme.UseMonochromeLogo) {
			ImVec4 textColor = menu->GetSettings().Theme.Palette.Text;
			textColor.w = 0.24f;  // Low alpha for watermark effect
			watermarkColor = ImGui::GetColorU32(textColor);
		} else {
			watermarkColor = IM_COL32(255, 255, 255, 60);
		}

		// Render as subtle watermark background
		ImGui::GetWindowDrawList()->AddImage(menu->uiIcons.logo.texture, logoMin, logoMax,
			ImVec2(0, 0), ImVec2(1, 1), watermarkColor);
	}

	// Center all content
	float windowWidth = ImGui::GetWindowWidth();
	auto centerText = [windowWidth](const char* text) {
		ImGui::SetCursorPosX((windowWidth - ImGui::CalcTextSize(text).x) * 0.5f);
	};
	auto centerWidth = [windowWidth](float width) {
		ImGui::SetCursorPosX((windowWidth - width) * 0.5f);
	};

	// Version text - two lines, both centered (reduced spacing between lines)
	const char* versionLine1 = T("menu.setup.new_install_line1", "This appears to be a new install, update, or");
	const char* versionLine2 = T("menu.setup.new_install_line2", "reinstallation of Bottled Shaders.");

	centerText(versionLine1);
	ImGui::Text("%s", versionLine1);
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() - DIALOG_LINE_TIGHTEN * uiScale);
	centerText(versionLine2);
	ImGui::Text("%s", versionLine2);

	ImGui::Spacing();

	// Description - centered
	const char* description = T("menu.setup.choose_hotkey", "Please choose a hotkey to access the menu:");
	centerText(description);
	ImGui::Text("%s", description);

	// Hotkey selection - clickable hotkey text
	// Show current toggle key and allow user to change it by clicking on it
	auto& themeSettings = menu->GetTheme();
	bool isCapturing = menu->settingToggleKey;

	// Increase font size for hotkey text - bigger when capturing
	ImGui::SetWindowFontScale(isCapturing ? HOTKEY_TEXT_SCALE_CAPTURING : HOTKEY_TEXT_SCALE);

	// Format hotkey with brackets to make it look like a button
	std::string hotkeyStr;
	if (isCapturing) {
		hotkeyStr = "[ ... ]";
	} else {
		auto& keys = menu->GetSettings().ToggleKey;
		hotkeyStr = std::string("[ ") + Util::Input::KeyIdToString(keys) + " ]";
	}

	ImVec2 hotkeyTextSize = ImGui::CalcTextSize(hotkeyStr.c_str());

	centerWidth(hotkeyTextSize.x);
	ImVec2 buttonPos = ImGui::GetCursorScreenPos();

	// Create invisible button for hover detection and clicking
	ImGui::PushID("HotkeyButton");
	bool clicked = ImGui::InvisibleButton("##HotkeyClick", hotkeyTextSize);
	bool hovered = ImGui::IsItemHovered();
	ImGui::PopID();

	// Set cursor position back for text rendering
	ImGui::SetCursorScreenPos(buttonPos);

	// Choose color based on state
	ImVec4 hotkeyColor;
	if (isCapturing) {
		// Pulsing effect using theme's hotkey color
		hotkeyColor = Util::GetPulsingColor(themeSettings.StatusPalette.CurrentHotkey);
	} else if (hovered) {
		hotkeyColor = ImVec4(themeSettings.StatusPalette.CurrentHotkey.x * HOTKEY_HOVER_DIM_FACTOR,
			themeSettings.StatusPalette.CurrentHotkey.y * HOTKEY_HOVER_DIM_FACTOR,
			themeSettings.StatusPalette.CurrentHotkey.z * HOTKEY_HOVER_DIM_FACTOR,
			themeSettings.StatusPalette.CurrentHotkey.w);
	} else {
		hotkeyColor = themeSettings.StatusPalette.CurrentHotkey;
	}

	ImGui::TextColored(hotkeyColor, "%s", hotkeyStr.c_str());

	ImGui::SetWindowFontScale(1.0f);

	// Handle click to start hotkey capture
	if (clicked && !isCapturing) {
		// Prevent starting capture if this click was caused by Enter key,
		// because we want Enter to close the dialog instead.
		if (!ImGui::IsKeyPressed(ImGuiKey_Enter))
			menu->settingToggleKey = true;
	}

	// Show hotkey capture message when in capture mode
	if (isCapturing) {
		const char* pressKeyText = T("menu.setup.press_any_key", "Press any key to set as toggle key...");
		centerText(pressKeyText);
		ImGui::TextDisabled("%s", pressKeyText);
	}

	// CS Editor hotkey status — updates live as user picks keys
	{
		auto& csEditorKey = menu->GetSettings().CSEditorToggleKey;
		if (csEditorKey.empty()) {
			const char* warnText = T("menu.setup.cs_editor_unbound", "CS Editor hotkey unbound - chosen key uses Shift");
			centerText(warnText);
			Util::Text::Warning("%s", warnText);
		} else {
			std::string infoStr = I18n::GetSingleton()->Format("menu.setup.cs_editor_will_be",
				{ { "key", Util::Input::KeyIdToString(csEditorKey) } },
				"CS Editor hotkey will be: {key}");
			centerText(infoStr.c_str());
			ImGui::TextDisabled("%s", infoStr.c_str());
		}
	}

	ImGui::Spacing();

	const char* laterText = T("menu.setup.change_later", "You can change this later in General > Keybindings.");
	centerText(laterText);
	ImGui::Text("%s", laterText);

	ImGui::Spacing();

	// Check for Enter or Escape key to close, but only if not capturing a hotkey
	bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
	if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || escapePressed) && !isCapturing) {
		MarkFirstTimeSetupComplete(escapePressed ? VK_ESCAPE : VK_RETURN);
	}

	// Help text with breathing animation
	const char* helpText = T("menu.setup.press_to_close", "Press Escape or Enter to continue");

	ImGui::SetWindowFontScale(HELP_TEXT_SCALE);
	centerText(helpText);
	Util::DrawBreathingText(helpText);

	ImGui::SetWindowFontScale(1.0f);

	ImGui::End();
	ImGui::PopStyleVar();
}

bool HomePageRenderer::ShouldShowFirstTimeSetup()
{
	// Check if already completed this session
	if (isFirstTimeSetupShown) {
		return false;
	}

	// Check if first-time setup has been completed using the Menu settings
	auto menu = Menu::GetSingleton();
	return !menu->GetSettings().FirstTimeSetupCompleted;
}

void HomePageRenderer::MarkFirstTimeSetupComplete(uint32_t closingKey)
{
	// Set the flag in the Menu settings
	auto menu = Menu::GetSingleton();
	menu->GetSettings().FirstTimeSetupCompleted = true;
	// Ensure we are not capturing a hotkey when closing the dialog
	menu->settingToggleKey = false;

	// Immediately save settings to ensure the flag is persisted
	// This prevents the welcome screen from showing again even if user doesn't manually save
	globals::state->Save();

	isFirstTimeSetupShown = true;
	keyThatClosedDialog = closingKey;
}
