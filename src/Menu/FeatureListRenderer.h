#pragma once

#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

struct Feature;

/**
 * @brief Renders the two-column feature list and settings panel in the main menu.
 *
 * The left column shows a searchable, categorized list of built-in pages and
 * installed features. The right column displays the settings UI for whichever
 * item is currently selected.
 */
class FeatureListRenderer
{
public:
	/** @brief Describes a built-in (non-feature) menu page with a name and draw callback. */
	struct BuiltInMenu
	{
		std::string name;
		std::function<void()> func;
	};

	/** @brief Represents a collapsible category header in the feature list. */
	struct CategoryHeader
	{
		std::string name;
	};

	/** @brief Variant type representing any entry in the menu list. */
	using MenuFuncInfo = std::variant<BuiltInMenu, std::string, CategoryHeader, Feature*>;

	/**
	 * @brief Renders the full two-column feature list and settings panel.
	 *
	 * Builds the menu list from built-in pages and loaded features, handles
	 * pending feature selection requests, then draws the left-column navigation
	 * and right-column settings content.
	 *
	 * @param footerHeight Height reserved for the footer area below the list.
	 * @param selectedMenu Index of the currently selected menu item (updated on selection change).
	 * @param featureSearch Current search filter string (updated by the search input).
	 * @param pendingFeatureSelection Name of a feature to auto-select (cleared after processing).
	 * @param categoryExpansionStates Map of category name to expanded/collapsed state.
	 * @param drawGeneralSettings Callback that renders the General settings page content.
	 * @param drawAdvancedSettings Callback that renders the Advanced settings page content.
	 */
	static void RenderFeatureList(
		float footerHeight,
		size_t& selectedMenu,
		std::string& featureSearch,
		std::string& pendingFeatureSelection,
		std::map<std::string, bool>& categoryExpansionStates,
		const std::function<void()>& drawGeneralSettings,
		const std::function<void()>& drawAdvancedSettings,
		const std::function<void()>& drawEditorSettings);

	/**
	 * @brief Renders the full settings panel for a single feature.
	 *
	 * Draws the same header (boot toggle, override and restore actions) and
	 * settings body used by the right-hand column, so features relocated onto
	 * the Editor and Advanced pages keep their complete UI.
	 *
	 * @param feat Feature whose panel should be drawn.
	 */
	static void RenderFeaturePanel(Feature* feat);

	/** @brief True when the feature is hosted on the Editor page instead of the feature list. */
	static bool IsEditorPageFeature(Feature* feat);

	/** @brief True when the feature is hosted on the Advanced page instead of the feature list. */
	static bool IsAdvancedPageFeature(Feature* feat);

	/** @brief Returns the loaded features hosted on the Editor page, sorted by display name. */
	static std::vector<Feature*> GetEditorPageFeatures();

	/** @brief Returns the loaded features hosted on the Advanced page, sorted by display name. */
	static std::vector<Feature*> GetAdvancedPageFeatures();

private:
	struct ListMenuVisitor
	{
		size_t listId;
		size_t& selectedMenuRef;
		std::map<std::string, bool>& categoryExpansionStates;

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string& label);
		void operator()(const CategoryHeader& header);
		void operator()(Feature* feat);
	};

	struct DrawMenuVisitor
	{
		explicit DrawMenuVisitor(std::string& pendingFeatureSelectionRef) :
			pendingFeatureSelection(pendingFeatureSelectionRef) {}

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string&);
		void operator()(const CategoryHeader&);
		void operator()(Feature* feat);

	private:
		std::string& pendingFeatureSelection;

		// Helper methods for Feature rendering
		void RenderFeatureHeader(Feature* feat, bool isDisabled, bool isLoaded, bool sceneControlled);
		void RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage, bool sceneControlled);
		void RenderReactiveConstraintWarningDialog();
	};

	static std::vector<MenuFuncInfo> BuildMenuList(
		const std::string& featureSearch,
		std::map<std::string, bool>& categoryExpansionStates,
		const std::function<void()>& drawGeneralSettings,
		const std::function<void()>& drawAdvancedSettings,
		const std::function<void()>& drawEditorSettings);

	static void HandlePendingFeatureSelection(
		std::string& pendingFeatureSelection,
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu);

	static void RenderLeftColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu,
		std::string& featureSearch,
		std::map<std::string, bool>& categoryExpansionStates);

	static void RenderRightColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t selectedMenu,
		std::string& pendingFeatureSelection);
};