#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Renders the Home page of the in-game menu with the masthead, FAQ, and first-time setup.
 *
 * Displays the brand masthead, quick links, active constraint warnings, and a
 * grouped FAQ section. Also manages the first-time setup dialog shown on
 * initial launch.
 */
class HomePageRenderer
{
public:
	// Constants
	static constexpr float TITLE_FONT_SCALE = 2.0f;
	static constexpr float HOTKEY_TEXT_SCALE = 1.6f;
	static constexpr float HOTKEY_TEXT_SCALE_CAPTURING = 2.0f;
	static constexpr float HOTKEY_HOVER_DIM_FACTOR = 0.7f;
	static constexpr float HELP_TEXT_SCALE = 1.35f;
	static constexpr float LOGO_WATERMARK_HEIGHT = 156.0f;
	static constexpr uint8_t MODAL_OVERLAY_ALPHA = 160;

	// First-time setup dialog layout (1080p baseline, scaled by GetUIScale)
	static constexpr float DIALOG_MIN_WIDTH = 390.0f;
	static constexpr float DIALOG_MAX_WIDTH = 470.0f;
	static constexpr float DIALOG_CORNER_ROUNDING = 6.0f;
	static constexpr float DIALOG_LINE_TIGHTEN = 3.0f;

	// Masthead layout (1080p baseline, scaled by GetUIScale)
	static constexpr float MASTHEAD_EMBLEM_HEIGHT = 88.0f;
	static constexpr float MASTHEAD_EMBLEM_ALPHA = 0.85f;
	static constexpr float SUBTITLE_ALPHA = 0.62f;

	// Ornamental divider rule, echoing Skyrim's tapered menu separators
	static constexpr float RULE_THICKNESS = 1.0f;
	static constexpr float RULE_DIAMOND_RADIUS = 3.5f;
	static constexpr float RULE_DIAMOND_GAP = 2.6f;   // Multiple of the diamond radius
	static constexpr float RULE_VERTICAL_PADDING = 6.0f;
	static constexpr float RULE_MIN_WIDTH = 120.0f;
	static constexpr float RULE_DEFAULT_WIDTH_RATIO = 0.62f;

	// FAQ layout
	static constexpr float FAQ_GROUP_TOP_SPACING = 10.0f;
	static constexpr float FAQ_ANSWER_INDENT = 12.0f;
	static constexpr float FAQ_GROUP_LABEL_ALPHA = 0.55f;

	/** @brief Renders the full Home page including welcome, quick links, constraints, and FAQ sections. */
	static void RenderHomePage();

	// First-time setup management
	/** @brief Returns true if the first-time setup dialog should be displayed to the user. */
	static bool ShouldShowFirstTimeSetup();

	/** @brief Renders the modal first-time setup dialog overlay with initial configuration options. */
	static void RenderFirstTimeSetupDialog();

	/**
	 * @brief Checks whether a key release event should be suppressed.
	 *
	 * Returns true and clears internal state if the given key was the one
	 * used to close the first-time setup dialog, preventing the release
	 * from triggering the menu toggle.
	 *
	 * @param key The virtual key code of the released key.
	 * @return true if the key release should be consumed, false otherwise.
	 */
	static bool ShouldSkipKeyRelease(uint32_t key);

private:
	static void RenderMastheadSection();
	static void RenderQuickLinksSection();
	static void RenderFAQSection();
	static void RenderActiveConstraintsSection();

	/** @brief Draws a centred, tapered divider with a small diamond finial. */
	static void DrawOrnamentalRule(float widthRatio = RULE_DEFAULT_WIDTH_RATIO);

	/** @brief Draws a centred section title followed by an ornamental rule. */
	static void DrawSectionTitle(const char* title);

	/** @brief Draws a dimmed, letter-spaced group label above a run of FAQ entries. */
	static void DrawFAQGroupLabel(const char* label);

	/** @brief Draws one collapsible FAQ entry with an indented, wrapped answer. */
	static void DrawFAQEntry(const char* question, const char* answer);

	static void MarkFirstTimeSetupComplete(uint32_t closingKey);

	// State
	static bool isFirstTimeSetupShown;
	static uint32_t keyThatClosedDialog;
};
