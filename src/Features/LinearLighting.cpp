#include "LinearLighting.h"

#include "../I18n/I18n.h"
#include "Effects11.h"
#include "Globals.h"
#include "State.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.linear_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LinearLighting::Settings,
	enableLinearLighting,
	lightGamma,
	colorGamma,
	emitColorGamma,
	glowmapGamma,
	ambientGamma,
	fogGamma,
	fogAlphaGamma,
	effectGamma,
	effectAlphaGamma,
	skyGamma,
	waterGamma,
	vlGamma,
	vanillaDiffuseColorMult,
	directionalLightMult,
	pointLightMult,
	ambientMult,
	emitColorMult,
	glowmapMult,
	effectLightingMult,
	membraneEffectMult,
	bloodEffectMult,
	projectedEffectMult,
	deferredEffectMult,
	otherEffectMult)

void LinearLighting::ApplyMasterGamma(float a_newMaster)
{
	const float delta = a_newMaster - settings.colorGamma;
	const auto shift = [delta](float& a_gamma) { a_gamma = std::clamp(a_gamma + delta, 0.1f, 3.0f); };
	shift(settings.lightGamma);
	shift(settings.colorGamma);
	shift(settings.emitColorGamma);
	shift(settings.glowmapGamma);
	shift(settings.ambientGamma);
	shift(settings.fogGamma);
	shift(settings.fogAlphaGamma);
	shift(settings.effectGamma);
	shift(settings.effectAlphaGamma);
	shift(settings.skyGamma);
	shift(settings.waterGamma);
	shift(settings.vlGamma);
}

void LinearLighting::UnifyGammas(float a_gamma)
{
	a_gamma = std::clamp(a_gamma, 0.1f, 3.0f);
	settings.lightGamma = a_gamma;
	settings.colorGamma = a_gamma;
	settings.emitColorGamma = a_gamma;
	settings.glowmapGamma = a_gamma;
	settings.ambientGamma = a_gamma;
	settings.fogGamma = a_gamma;
	settings.fogAlphaGamma = a_gamma;
	settings.effectGamma = a_gamma;
	settings.effectAlphaGamma = a_gamma;
	settings.skyGamma = a_gamma;
	settings.waterGamma = a_gamma;
	settings.vlGamma = a_gamma;
}

void LinearLighting::SetLightingBrightness(float a_brightness)
{
	settings.directionalLightMult = a_brightness;
	settings.pointLightMult = a_brightness;
	settings.ambientMult = a_brightness;
}

void LinearLighting::ApplyPresetMaster(float a_master)
{
	const bool wasEnabled = settings.enableLinearLighting;
	RestoreDefaultSettings();
	settings.enableLinearLighting = wasEnabled;
	ApplyMasterGamma(a_master);
}

void LinearLighting::DrawSettings()
{
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Warning, "Settings are currently managed by ENB.");
			ImGui::SeparatorText(T(TKEY("effect_brightness_enb"), "Effect Brightness (works under ENB)"));
			ImGui::SliderFloat(T(TKEY("other_effects_multiplier"), "Other Effects Multiplier"), &settings.otherEffectMult, 0.0f, 10.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("other_effects_multiplier_tooltip"), "Brightness of additive effects such as fire and magic. Raise to compensate for dim flames under ENB."));
			ImGui::SliderFloat(T(TKEY("deferred_effects_multiplier"), "Deferred Effects Multiplier"), &settings.deferredEffectMult, 0.0f, 10.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("deferred_effects_multiplier_tooltip"), "Brightness of effects rendered through the deferred path. Pair with Other Effects Multiplier if some fire still reads dim."));
			return;
		}
	}

	ImGui::Checkbox(T(TKEY("enable"), "Enable Linear Lighting"), (bool*)&settings.enableLinearLighting);

	if (ImGui::BeginTabBar("##LinearLightingTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_simple"), "Simple"))) {
			ImGui::TextWrapped("%s", T(TKEY("simple_help"), "Quick controls that affect the whole image. For per-domain control, use the General and Advanced tabs."));

			ImGui::SeparatorText(T(TKEY("presets"), "Presets"));
			if (ImGui::Button(T(TKEY("preset_vanilla"), "Vanilla Match")))
				ApplyPresetMaster(1.8f);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("preset_vanilla_tooltip"), "The shipped default balance. Closest match to the vanilla look."));
			ImGui::SameLine();
			if (ImGui::Button(T(TKEY("preset_srgb"), "sRGB (2.2)")))
				ApplyPresetMaster(2.2f);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("preset_srgb_tooltip"), "Standard sRGB response. Deeper midtones and more contrast."));
			ImGui::SameLine();
			if (ImGui::Button(T(TKEY("preset_soft"), "Soft (1.5)")))
				ApplyPresetMaster(1.5f);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("preset_soft_tooltip"), "Lifted midtones for a softer, brighter look."));

			ImGui::SeparatorText(T(TKEY("quick_adjust"), "Quick Adjust"));

			float master = settings.colorGamma;
			if (ImGui::SliderFloat(T(TKEY("master_gamma"), "Master Gamma"), &master, 0.1f, 3.0f, "%.2f"))
				ApplyMasterGamma(master);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("master_gamma_tooltip"), "Shifts every gamma curve together, preserving the relative balance between domains. Higher = darker midtones."));

			float brightness = settings.directionalLightMult;
			if (ImGui::SliderFloat(T(TKEY("lighting_brightness"), "Lighting Brightness"), &brightness, 0.0f, 5.0f, "%.2f"))
				SetLightingBrightness(brightness);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lighting_brightness_tooltip"), "Scales directional, point, and ambient light together."));

			ImGui::Spacing();
			if (ImGui::Button(T(TKEY("unify_gammas"), "Match all domains to Master")))
				UnifyGammas(master);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("unify_gammas_tooltip"), "Sets every domain to the same gamma so all surfaces share one response curve - the most reliable way to make everything match."));

			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_general"), "General"))) {
			ImGui::SeparatorText(T(TKEY("gamma_settings"), "Gamma Settings"));
			ImGui::SliderFloat(T(TKEY("fog_gamma"), "Fog Gamma"), &settings.fogGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("fog_transparency_gamma"), "Fog Transparency Gamma"), &settings.fogAlphaGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("sky_gamma"), "Sky Gamma"), &settings.skyGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("vl_gamma"), "Volumetric Lighting Gamma"), &settings.vlGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("water_gamma"), "Water Gamma"), &settings.waterGamma, 0.1f, 3.0f, "%.2f");

			ImGui::SeparatorText(T(TKEY("multipliers"), "Multipliers"));
			ImGui::SliderFloat(T(TKEY("directional_light_multiplier"), "Directional Light Multiplier"), &settings.directionalLightMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ambient_multiplier"), "Ambient Multiplier"), &settings.ambientMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("glowmap_multiplier"), "Glowmap Multiplier"), &settings.glowmapMult, 0.0f, 10.0f, "%.2f");

			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_advanced"), "Advanced"))) {
			ImGui::SeparatorText(T(TKEY("gamma_settings"), "Gamma Settings"));
			ImGui::SliderFloat(T(TKEY("light_gamma"), "Light Gamma"), &settings.lightGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("color_gamma"), "Color Gamma"), &settings.colorGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("emissive_color_gamma"), "Emissive Color Gamma"), &settings.emitColorGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("glowmap_gamma"), "Glowmap Gamma"), &settings.glowmapGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ambient_gamma"), "Ambient Gamma"), &settings.ambientGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("effect_gamma"), "Effect Gamma"), &settings.effectGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("effect_transparency_gamma"), "Effect Transparency Gamma"), &settings.effectAlphaGamma, 0.1f, 3.0f, "%.2f");

			ImGui::SeparatorText(T(TKEY("multipliers"), "Multipliers"));
			ImGui::SliderFloat(T(TKEY("vanilla_diffuse_color_multiplier"), "Vanilla Diffuse Color Multiplier"), &settings.vanillaDiffuseColorMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("emissive_color_multiplier"), "Emissive Color Multiplier"), &settings.emitColorMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("point_light_multiplier"), "Point Light Multiplier"), &settings.pointLightMult, 0.0f, 10.0f, "%.2f");

			if (ImGui::TreeNodeEx(T(TKEY("effects"), "Effects"), ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderFloat(T(TKEY("effect_lighting_multiplier"), "Effect Lighting Multiplier"), &settings.effectLightingMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("membrane_effects_multiplier"), "Membrane Effects Multiplier"), &settings.membraneEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("blood_effects_multiplier"), "Blood Effects Multiplier"), &settings.bloodEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("projected_effects_multiplier"), "Projected Effects Multiplier"), &settings.projectedEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("deferred_effects_multiplier"), "Deferred Effects Multiplier"), &settings.deferredEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("other_effects_multiplier"), "Other Effects Multiplier"), &settings.otherEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::TreePop();
			}

			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void LinearLighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LinearLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LinearLighting::RestoreDefaultSettings()
{
	settings = {};
}

void LinearLighting::SetupResources()
{
	PerGeometryCB = new ConstantBuffer(ConstantBufferDesc<PerGeometryData>());
}

void LinearLighting::Prepass()
{
	bool isMainLoadingMenu = globals::state->isMainMenuOpen || globals::state->isLoadingMenuOpen;
	dirLightMult = 1.0f;
	if (!settings.enableLinearLighting || isMainLoadingMenu)
		return;

	auto imageSpaceManager = globals::game::imageSpaceManager;
	if (!imageSpaceManager)
		return;

	dirLightMult = imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale;
}

struct LinearLighting::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			globals::features::linearLighting.BSLightingShader_SetupGeometry(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		logger::info("[LinearLighting] Installed hooks - BSLightingShader_SetupGeometry");
	}
};

void LinearLighting::PostPostLoad()
{
	LinearLighting::Hooks::Install();
}

LinearLighting::PerFrameData LinearLighting::GetCommonBufferData()
{
	if (!loaded) {
		auto data = PerFrameData{};
		data.enableLinearLighting = false;
		return data;
	}
	bool isMainLoadingMenu = globals::state->isMainMenuOpen || globals::state->isLoadingMenuOpen;
	auto data = PerFrameData{};
	data.enableLinearLighting = settings.enableLinearLighting && !isMainLoadingMenu;
	data.isDirLightLinear = isDirLightLinear;
	data.dirLightMult = dirLightMult;
	data.lightGamma = settings.lightGamma;
	data.colorGamma = settings.colorGamma;
	data.emitColorGamma = settings.emitColorGamma;
	data.glowmapGamma = settings.glowmapGamma;
	data.ambientGamma = settings.ambientGamma;
	data.fogGamma = settings.fogGamma;
	data.fogAlphaGamma = settings.fogAlphaGamma;
	data.effectGamma = settings.effectGamma;
	data.effectAlphaGamma = settings.effectAlphaGamma;
	data.skyGamma = settings.skyGamma;
	data.waterGamma = settings.waterGamma;
	data.vlGamma = settings.vlGamma;
	data.vanillaDiffuseColorMult = settings.vanillaDiffuseColorMult;
	data.directionalLightMult = settings.directionalLightMult;
	data.pointLightMult = settings.pointLightMult;
	data.ambientMult = settings.ambientMult;
	data.emitColorMult = settings.emitColorMult;
	data.glowmapMult = settings.glowmapMult;
	data.effectLightingMult = settings.effectLightingMult;
	data.membraneEffectMult = settings.membraneEffectMult;
	data.bloodEffectMult = settings.bloodEffectMult;
	data.projectedEffectMult = settings.projectedEffectMult;
	data.deferredEffectMult = settings.deferredEffectMult;
	data.otherEffectMult = settings.otherEffectMult;

	// When ENB post-processing is active, ENB takes over linear lighting:
	// neutralize all gammas and multipliers so we pass through untouched.
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			data.enableLinearLighting = false;
			data.lightGamma = 1.0f;
			data.colorGamma = 1.0f;
			data.emitColorGamma = 1.0f;
			data.glowmapGamma = 1.0f;
			data.ambientGamma = 1.0f;
			data.fogGamma = 1.0f;
			data.fogAlphaGamma = 1.0f;
			data.effectGamma = 1.0f;
			data.effectAlphaGamma = 1.0f;
			data.skyGamma = 1.0f;
			data.vlGamma = 1.0f;
			data.vanillaDiffuseColorMult = 1.0f;
			data.directionalLightMult = 1.0f;
			data.pointLightMult = 1.0f;
			data.ambientMult = 1.0f;
			data.emitColorMult = 1.0f;
			data.glowmapMult = 1.0f;
			data.effectLightingMult = 1.0f;
			data.membraneEffectMult = 1.0f;
			data.bloodEffectMult = 1.0f;
			data.projectedEffectMult = 1.0f;
		}
	}
	return data;
}

RE::NiColor LinearLighting::ColorToLinear(RE::NiColor inColor, float gamma)
{
	RE::NiColor outColor;
	outColor.red = std::pow(inColor.red, gamma);
	outColor.green = std::pow(inColor.green, gamma);
	outColor.blue = std::pow(inColor.blue, gamma);
	return outColor;
}

void LinearLighting::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto& property1 = a_pass->geometry->GetGeometryRuntimeData().shaderProperty;
	auto lightProperty = property1 && property1->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ? static_cast<RE::BSLightingShaderProperty*>(property1.get()) : nullptr;

	if (lightProperty != nullptr) {
		float emissiveMult = 1.0f;
		if (settings.enableLinearLighting) {
			emissiveMult = lightProperty->emissiveMult;
			PerGeometryData perGeometryData{};
			perGeometryData.emissiveMult = emissiveMult;
			PerGeometryCB->Update(perGeometryData);

			ID3D11Buffer* buffer = { PerGeometryCB->CB() };
			auto context = globals::d3d::context;
			context->PSSetConstantBuffers(8, 1, &buffer);
		}
	}
}

#undef I18N_KEY_PREFIX
