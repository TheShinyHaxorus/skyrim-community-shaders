#include "Upscaling.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "HDRDisplay.h"
#include "Hooks.h"
#include "PostProcessing.h"
#include "RenderDoc.h"
#include "State.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/FidelityFX/DriverFsr4.h"
#include "Upscaling/Streamline.h"
#include "Utils/Game.h"
#include "Utils/UI.h"
#include "Utils/VersionedRelocation.h"
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <directx/d3dx12.h>
#include <format>

#include "Features/PostProcessing.h"

#define I18N_KEY_PREFIX "feature.upscaling."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::Settings,
	upscaleMethod,
	upscaleMethodNoDLSS,
	qualityMode,
	frameLimitMode,
	frameGenerationMode,
	frameGenerationForceEnable,
	frameGenerationAllowInMenus,
	frameGenerationFPSLimitEnabled,
	frameGenerationFPSLimit,
	streamlineLogLevel,
	sharpnessFSR,
	sharpnessEnabledDLSS,
	sharpnessDLSS,
	presetDLSS,
	dlssHintMasks,
	neuralRenderingEnabled,
	neuralRendering,
	reflexLowLatencyMode,
	reflexLowLatencyBoost,
	reflexUseMarkersToOptimize,
	reflexUseFPSLimit,
	reflexFPSLimit,
	fsr4RuntimeEnable,
	fsr4DriverModel,
	fsr4RuntimeSelectionSchemaVersion);

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChainUpscaling;

void ApplyLegacyFsr4RuntimeSelectionMigration(Upscaling::Settings& a_settings, FidelityFX::Fsr4AdapterSupport a_adapterSupport)
{
	if (a_settings.fsr4RuntimeSelectionSchemaVersion >= Upscaling::kFsr4RuntimeSelectionSchemaVersion)
		return;

	if (a_adapterSupport == FidelityFX::Fsr4AdapterSupport::Unsupported)
		return;

	a_settings.fsr4RuntimeSelectionSchemaVersion = Upscaling::kFsr4RuntimeSelectionSchemaVersion;
}

/**
 * @brief Creates a Direct3D 11 device and swap chain, with support for advanced upscaling and frame generation features.
 *
 * This function intercepts the standard D3D11 device and swap chain creation process to enable integration with Streamline and FidelityFX technologies, as well as optional D3D12 proxying for frame generation. It adjusts swap chain flags for tearing support, manages feature checks, and conditionally routes device creation through Streamline or FidelityFX proxies based on runtime settings and hardware capabilities. If frame generation is enabled and supported, a D3D12 proxy is used; otherwise, the standard D3D11 creation path is followed.
 *
 * @return HRESULT indicating the success or failure of device and swap chain creation.
 */
HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChainUpscaling(
	IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	[[maybe_unused]] const D3D_FEATURE_LEVEL* pFeatureLevels,
	[[maybe_unused]] UINT FeatureLevels,
	UINT SDKVersion,
	DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	auto& upscaling = globals::features::upscaling;

	DXGI_ADAPTER_DESC adapterDesc{};
	if (pAdapter && SUCCEEDED(pAdapter->GetDesc(&adapterDesc))) {
		globals::state->SetAdapterDescription(adapterDesc.Description);
		ApplyLegacyFsr4RuntimeSelectionMigration(upscaling.settings, FidelityFX::GetFsr4AdapterSupport(adapterDesc));
	}

	upscaling.LoadUpscalingSDKs();

	// FLIP_DISCARD requires BufferCount >= 2 and a flip-model-compatible (non-sRGB) format.
	pSwapChainDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (pSwapChainDesc->BufferCount < 2)
		pSwapChainDesc->BufferCount = 2;

	if (globals::features::hdrDisplay.loaded) {
		logger::info("[Upscaling] Upgrading swap chain format from {} to R10G10B10A2_UNORM for HDR", static_cast<int>(pSwapChainDesc->BufferDesc.Format));
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	} else if (pSwapChainDesc->BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	} else if (pSwapChainDesc->BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	bool shouldProxy = true;
	if (shouldProxy)
		if (!pSwapChainDesc->Windowed)
			shouldProxy = false;

	auto refreshRate = Upscaling::GetRefreshRate(pSwapChainDesc->OutputWindow);
	upscaling.refreshRate = refreshRate;

	if (shouldProxy) {
		if (upscaling.settings.frameGenerationMode)
			if (refreshRate >= 120)
				shouldProxy = true;
			else if (upscaling.settings.frameGenerationForceEnable)
				shouldProxy = true;
			else
				shouldProxy = false;
		else
			shouldProxy = false;
	}

	if (shouldProxy && Streamline::IsSmoothMotionEnabledForProfile()) {
		logger::warn("[Frame Generation] NVIDIA Smooth Motion is enabled; disabling this plugin's frame generation to avoid crashing alongside it");
		shouldProxy = false;
	}

	upscaling.lowRefreshRate = refreshRate < 120;
	upscaling.isWindowed = pSwapChainDesc->Windowed;

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;

	if (shouldProxy) {
		logger::info("[Frame Generation] Frame Generation enabled, using D3D12 proxy");

		if (upscaling.HasFrameGenModule()) {
			DX::ThrowIfFailed(D3D11CreateDevice(
				pAdapter,
				DriverType,
				Software,
				Flags,
				&featureLevel,
				1,
				SDKVersion,
				ppDevice,
				pFeatureLevel,
				ppImmediateContext));

			upscaling.SetProxyD3D11Device(*ppDevice);
			upscaling.SetProxyD3D11DeviceContext(*ppImmediateContext);
			upscaling.CreateProxySwapChain(pAdapter, *pSwapChainDesc);
			upscaling.CreateProxyInterop();

			*ppSwapChain = upscaling.GetProxySwapChain();

			upscaling.d3d12SwapChainActive = true;

			if (upscaling.IsBackendInitialized()) {
				upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
				// Don't wrap the swap chain with Streamline when using the D3D12
				// proxy.  The proxy's GetDevice() returns the D3D11 device for
				// IID_ID3D11Device, which other SKSE plugins (e.g. SkyrimPlatform)
				// rely on.  Streamline's wrapper would bypass this override and
				// forward to the underlying D3D12 swap chain, causing
				// E_NOINTERFACE.  The proxy must remain the outermost layer.
				upscaling.SetBackendD3DDevice(*ppDevice);
				// Feature availability (notably Reflex/PCL) is only reliable after device bind.
				upscaling.CheckBackendFeatures(pAdapter);
				upscaling.PostBackendDevice();
			}

			return S_OK;
		} else {
			logger::warn("[Frame Generation] FidelityFX DLLs are not loaded, skipping proxy");
			upscaling.fidelityFXMissing = true;
		}
	}

	auto ret = ptrD3D11CreateDeviceAndSwapChainUpscaling(pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);

	if (upscaling.IsBackendInitialized()) {
		upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
		upscaling.UpgradeBackendInterface((void**)&(*ppSwapChain));
		upscaling.SetBackendD3DDevice(*ppDevice);
		// Feature availability (notably Reflex/PCL) is only reliable after device bind.
		upscaling.CheckBackendFeatures(pAdapter);
		upscaling.PostBackendDevice();
	}

	return ret;
}

void Upscaling::DrawSettings()
{
	// Display upscaling options in the UI
	std::vector<std::string> upscaleModes = {
		T(TKEY("method_none"), "None"),
		T(TKEY("method_taa"), "TAA")
	};

	std::string fsrLabel = "AMD FSR 3.1";
	upscaleModes.push_back(fsrLabel);

	std::string dlssLabel = "NVIDIA DLSS";
	upscaleModes.push_back(dlssLabel);

	// Determine available modes
	bool featureDLSS = streamline.featureDLSS;
	bool featureFSR = true;  // FSR is always available

	uint32_t* currentUpscaleMode = &settings.upscaleMethod;
	uint32_t availableModes = 1;  // Start with TAA
	if (featureFSR)
		availableModes = 2;  // Add FSR
	if (featureDLSS)
		availableModes = 3;  // Add DLSS if available
	else
		currentUpscaleMode = &settings.upscaleMethodNoDLSS;

	// Dropdown for method selection
	std::vector<const char*> modeLabels;
	for (uint32_t i = 0; i <= availableModes; ++i)
		modeLabels.push_back(upscaleModes[i].c_str());
	ImGui::Combo(T(TKEY("method"), "Method"), (int*)currentUpscaleMode, modeLabels.data(), (int)modeLabels.size());

	*currentUpscaleMode = std::min(availableModes, *currentUpscaleMode);

	// Check the current upscale method
	auto upscaleMethod = GetUpscaleMethod();

	if (*currentUpscaleMode == (uint)UpscaleMethod::kFSR && upscaleMethod == UpscaleMethod::kTAA)
		Util::Text::Warning("%s", T(TKEY("fsr_renderdoc_fallback"), "AMD FSR is unavailable while RenderDoc is loaded, using TAA."));

	// Display warning for DLSS resolution limits
	if (upscaleMethod == UpscaleMethod::kDLSS) {
		float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
		if (screenSize.x > streamline.MAX_RESOLUTION || screenSize.y > streamline.MAX_RESOLUTION) {
			Util::Text::Warning("Warning: Requested resolution %.0f x %.0f exceeds maximum supported resolution %d x %d for DLSS.",
				screenSize.x, screenSize.y, streamline.MAX_RESOLUTION, streamline.MAX_RESOLUTION);
			Util::Text::Warning("DLSS will not function. Lower your resolution or select a different upscaling method.");
		}
	}

	// Display upscaling settings if applicable
	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		const char* upscalePresetsDLSS[] = {
			T(TKEY("preset_ultra_performance"), "Ultra Performance"),
			T(TKEY("preset_performance"), "Performance"),
			T(TKEY("preset_balanced"), "Balanced"),
			T(TKEY("preset_quality"), "Quality"),
			T(TKEY("preset_dlaa"), "DLAA")
		};
		const char* upscalePresets[] = {
			T(TKEY("preset_ultra_performance"), "Ultra Performance"),
			T(TKEY("preset_performance"), "Performance"),
			T(TKEY("preset_balanced"), "Balanced"),
			T(TKEY("preset_quality"), "Quality"),
			T(TKEY("preset_native_aa"), "Native AA")
		};

		// Compute a safe preset index (4 - qualityMode) clamped to [0,4] to avoid negative/overflow indexing
		int presetIndex = 0;
		if (settings.qualityMode <= 4)
			presetIndex = 4 - static_cast<int>(settings.qualityMode);
		presetIndex = std::clamp(presetIndex, 0, 4);

		// Choose preset name set and the corresponding scales once, then show a
		// single SliderInt to avoid duplicated calls.
		const char* baseLabel = nullptr;

		if (upscaleMethod == UpscaleMethod::kFSR) {
			baseLabel = upscalePresets[presetIndex];
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			baseLabel = upscalePresetsDLSS[presetIndex];
		}

		if (baseLabel) {
			// Format the label with preset name and resolution scale
			std::string labelWithScale = std::format("{} ( {:.2f}x )", baseLabel, (resolutionScale.x + resolutionScale.y) * 0.5f);

			ImGui::SliderInt(T(TKEY("upscale_preset"), "Upscale Preset"), (int*)&settings.qualityMode, 0, 4, labelWithScale.c_str(), ImGuiSliderFlags_AlwaysClamp);
		}

		if (upscaleMethod == UpscaleMethod::kFSR) {
			ImGui::SliderFloat(T(TKEY("sharpness"), "Sharpness"), &settings.sharpnessFSR, 0.0f, 1.0f, "%.1f");
			// Hidden entirely on ineligible GPUs so it can't be toggled somewhere it silently no-ops.
			const bool driverFsr4 = fidelityFX.IsDriverFsr4Candidate();
			if (fidelityFX.IsRuntimeFsr4AutoEligible() || driverFsr4) {
				ImGui::Checkbox(T(TKEY("fsr4_runtime_enable"), "Use Runtime FSR4"), &settings.fsr4RuntimeEnable);
				if (driverFsr4) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted(T(TKEY("fsr4_driver_tooltip_1"), "This GPU's driver has no FSR 4, so it comes from AMD's driver library (amdxcffx64.dll, FSR 4.1.2) in the FidelityFX folder."));
						ImGui::TextUnformatted(T(TKEY("fsr4_driver_tooltip_2"), "Runs an INT8 model. Falls back to FSR 3.1 if the library will not load."));
					}
					const char* driverModels[] = {
						T(TKEY("fsr4_driver_model_int8"), "INT8"),
						T(TKEY("fsr4_driver_model_int8_lite"), "INT8 Lite")
					};
					int driverModel = static_cast<int>(std::min(settings.fsr4DriverModel, 1u));
					if (ImGui::Combo(T(TKEY("fsr4_driver_model"), "FSR4 Model"), &driverModel, driverModels, IM_ARRAYSIZE(driverModels)))
						settings.fsr4DriverModel = static_cast<uint>(driverModel);
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::TextUnformatted(T(TKEY("fsr4_driver_model_tooltip"), "INT8 is the full RDNA 3 network. INT8 Lite is the smaller network for integrated GPUs: faster, softer. Takes effect after a restart."));
					if (DriverFsr4::IsAvailable()) {
						const char* active = DriverFsr4::ActiveModelName();
						ImGui::TextDisabled("%s: %s%s%s", T(TKEY("fsr4_driver_provider"), "Driver library"), DriverFsr4::VersionName().c_str(), active ? ", " : "", active ? active : "");
					} else if (!DriverFsr4::FailureReason().empty()) {
						Util::Text::Warning("%s: %s", T(TKEY("fsr4_driver_failed"), "Driver FSR4 unavailable"), DriverFsr4::FailureReason().c_str());
					}
				}
				if (settings.fsr4RuntimeEnable != fsr4RuntimeEnableBoot)
					Util::Text::Warning("%s", T(TKEY("fsr4_restart_required"), "Restart the game to apply this change."));
				if (settings.fsr4RuntimeEnable) {
					ImGui::TextDisabled("%s: %s", T(TKEY("fsr4_active_path"), "Active path"), fidelityFX.GetDisplayedFsrPathLabel().c_str());
					if (fidelityFX.IsRuntimeFsr4FailureLatched())
						Util::Text::Warning("%s", T(TKEY("fsr4_failed_fallback"), "Runtime FSR4 failed this session -- using FSR3 fallback."));
					else if (fidelityFX.IsRuntimeUpscalerFailureLatched())
						Util::Text::Warning("%s", T(TKEY("fsr4_runtime_failed_fallback"), "Runtime upscaler DLL failed this session -- using host FSR3 SDK."));
				}
			}
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			ImGui::Checkbox(T(TKEY("dlss_hint_masks"), "DLSS Hint Masks"), &settings.dlssHintMasks);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("dlss_hint_masks_tooltip_1"), "Sends the game's TAA mask and the transparency mask to DLSS as bias and transparency hints."));
				ImGui::TextUnformatted(T(TKEY("dlss_hint_masks_tooltip_2"), "On by default. Turn off if skin and eyes show dark speckles below Native AA; turn back on if particles or water ghost."));
			}
			ImGui::Checkbox(T(TKEY("enable_sharpening"), "Enable Sharpening"), &settings.sharpnessEnabledDLSS);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("enable_sharpening_tooltip"),
									  "Applies RCAS sharpening to the DLSS output.\n"
									  "Off by default; DLSS already resolves a sharp image."));
			}

			if (settings.sharpnessEnabledDLSS)
				ImGui::SliderFloat(T(TKEY("sharpness"), "Sharpness"), &settings.sharpnessDLSS, 0.0f, 1.0f, "%.1f");

			const char* presets[] = {
				T(TKEY("dlss_model_preset_default"), "Default"),
				T(TKEY("dlss_model_preset_j"), "Preset J"),
				T(TKEY("dlss_model_preset_k"), "Preset K"),
				T(TKEY("dlss_model_preset_l"), "Preset L"),
				T(TKEY("dlss_model_preset_m"), "Preset M")
			};
			ImGui::Combo(T(TKEY("dlss_model_preset"), "DLSS Model Preset"), (int*)&settings.presetDLSS, presets, 5);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("dlss_model_preset_tooltip"),
									  "Choose which DLSS AI model preset to use.\n"
									  "Each model offers different visual quality, performance, and motion stability.\n"
									  "Set to 'Default' for automatic selection based on your Upscale Preset and hardware.\n"
									  "Changing this setting requires a restart to take effect."));
			}
		}
	}

	if (ImGui::TreeNodeEx(T(TKEY("frame_generation"), "Frame Generation"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::PushTextWrapPos(0.0f);

		bool fgEnabled = settings.frameGenerationMode != 0;
		if (ImGui::Checkbox(T(TKEY("frame_generation"), "Frame Generation"), &fgEnabled))
			settings.frameGenerationMode = fgEnabled ? 1 : 0;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frame_generation_tooltip"),
								  "Enable AMD FSR frame generation for smoother motion.\n"
								  "Requires windowed mode."));

		ImGui::Indent();
		{
			auto disabled = Util::DisableGuard(!fgEnabled);
			bool fgForce = settings.frameGenerationForceEnable != 0;
			if (ImGui::Checkbox(T(TKEY("force_enable_frame_generation"), "Allow frame generation below 120 Hz"), &fgForce))
				settings.frameGenerationForceEnable = fgForce ? 1 : 0;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("force_enable_frame_generation_tooltip"),
									  "Bypass the high-refresh-rate monitor check so Frame Generation can run on lower-Hz\n"
									  "displays. Useful for laptops and older monitors at the cost of less headroom for the\n"
									  "generated frames."));
			ImGui::Text(T(TKEY("frame_limit_refresh_rate"), "Detected refresh rate: %.2f Hz"), refreshRate);
			if (fgEnabled && lowRefreshRate && !settings.frameGenerationForceEnable)
				Util::Text::WrappedWarning("%s", T(TKEY("fg_warn_refresh_rate"), "Enable the option above to use frame generation on displays below 120 Hz."));
		}
		ImGui::Unindent();

		const bool fgLoaded = IsFrameGenerationDx12PathActive();
		if (fgEnabled != fgLoaded)
			Util::Text::WrappedWarning("%s", T(TKEY("fg_restart_required"), "Restart the game to apply the frame generation changes above."));

		ImGui::SeparatorText(T(TKEY("fg_current_session"), "Current session"));
		if (!fgEnabled)
			ImGui::TextDisabled("%s", T(TKEY("fg_disabled"), "Frame generation is off."));
		else if (!fgLoaded)
			Util::Text::WrappedWarning("%s", T(TKEY("fg_not_loaded"), "Frame generation is not loaded. Check the setup above and restart."));

		if (fgEnabled && !isWindowed)
			Util::Text::Warning("%s", T(TKEY("fg_warn_windowed"), "Warning: Requires windowed mode"));
		if (fgEnabled && fidelityFXMissing)
			Util::Text::Warning("%s", T(TKEY("fg_warn_fidelityfx_missing"), "Warning: FidelityFX DLLs are not loaded"));

		if (fgLoaded) {
			ImGui::TextUnformatted(T(TKEY("frame_generation_fsr_active"), "Method: AMD FSR"));
			ImGui::Text("%s", T(TKEY("fsr_frame_gen_fixed_multiplier"), "AMD FSR Frame Generation: Fixed 2x"));

			bool flEnabled = settings.frameLimitMode != 0;
			if (ImGui::Checkbox(T(TKEY("frame_limit_vrr"), "Limit FPS to display refresh rate"), &flEnabled))
				settings.frameLimitMode = flEnabled ? 1 : 0;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("fg_fsr_frame_limit_tooltip"),
					"Caps FSR's total FPS to the display refresh rate.\n"
					"When generation pauses, caps rendered FPS instead."));
		}

		ImGui::SeparatorText(T(TKEY("fg_shared_options"), "Options"));
		{
			auto disabled = Util::DisableGuard(!fgEnabled);
			ImGui::Checkbox(T(TKEY("frame_generation_in_menus"), "Frame Generation in Menus"), &settings.frameGenerationAllowInMenus);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("frame_generation_in_menus_tooltip_1"), "Keeps frame generation active while game menus are open."));
				ImGui::TextUnformatted(T(TKEY("frame_generation_in_menus_tooltip_2"), "May feel smoother, but increases menu input latency."));
			}

			ImGui::Checkbox(T(TKEY("frame_generation_fps_limit"), "Frame Generation FPS Limit"), &settings.frameGenerationFPSLimitEnabled);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("frame_generation_fps_limit_tooltip_1"), "A limit on the presented frame rate that only applies while frame generation is on."));
				ImGui::TextUnformatted(T(TKEY("frame_generation_fps_limit_tooltip_2"), "The rendered rate is half this value (FSR frame generation is 2x). The lower of this and the refresh-rate limit wins."));
			}
			if (settings.frameGenerationFPSLimitEnabled) {
				if (!std::isfinite(settings.frameGenerationFPSLimit))
					settings.frameGenerationFPSLimit = 120.0f;
				settings.frameGenerationFPSLimit = std::clamp(settings.frameGenerationFPSLimit, 30.0f, 480.0f);
				ImGui::SliderFloat(T(TKEY("frame_generation_fps_limit_value"), "Presented FPS"), &settings.frameGenerationFPSLimit, 30.0f, 480.0f, "%.0f");
			}
		}

		ImGui::PopTextWrapPos();
		ImGui::TreePop();
	}

	if (upscaleMethod == UpscaleMethod::kDLSS && ImGui::TreeNodeEx(T(TKEY("neural_rendering_section"), "DLSS 5 Neural Rendering"))) {
		ImGui::PushTextWrapPos(0.0f);
		DrawNeuralRenderingSettings();
		ImGui::PopTextWrapPos();
		ImGui::TreePop();
	}

	if (streamline.reflexSupportedOnCurrentAdapter && ImGui::TreeNodeEx(T(TKEY("nvidia_reflex"), "NVIDIA Reflex"), ImGuiTreeNodeFlags_DefaultOpen)) {
		const bool reflexAvailable = streamline.initialized && streamline.featureReflex;
		const bool markerOptimizationAvailable = reflexAvailable && streamline.featurePCL;

		if (!reflexAvailable) {
			ImGui::TextDisabled("%s", T(TKEY("reflex_not_available"), "Reflex is not available. Ensure sl.reflex.dll is present and restart."));
		}

		if (!reflexAvailable)
			ImGui::BeginDisabled();

		ImGui::Checkbox(T(TKEY("low_latency_mode"), "Low Latency Mode"), &settings.reflexLowLatencyMode);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("low_latency_mode_tooltip_1"), "Cuts input delay by syncing CPU work closer to the GPU."));
			ImGui::TextUnformatted(T(TKEY("low_latency_mode_tooltip_2"), "Can reduce max FPS a little, but usually feels more responsive."));
		}

		if (!settings.reflexLowLatencyMode)
			ImGui::BeginDisabled();

		ImGui::Checkbox(T(TKEY("low_latency_boost"), "Low Latency Boost"), &settings.reflexLowLatencyBoost);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("low_latency_boost_tooltip_1"), "Keeps GPU clocks higher to avoid latency spikes at low GPU load."));
			ImGui::TextUnformatted(T(TKEY("low_latency_boost_tooltip_2"), "Useful if frametime jumps; costs extra power and heat."));
		}

		if (!markerOptimizationAvailable)
			ImGui::BeginDisabled();

		ImGui::Checkbox(T(TKEY("use_markers_to_optimize"), "Use Markers To Optimize"), &settings.reflexUseMarkersToOptimize);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("use_markers_to_optimize_tooltip_1"), "Uses frame markers for tighter Reflex timing."));
			ImGui::TextUnformatted(T(TKEY("use_markers_to_optimize_tooltip_2"), "Try On first; turn Off if it causes stutter on your setup."));
		}

		if (!markerOptimizationAvailable)
			ImGui::EndDisabled();

		if (!markerOptimizationAvailable) {
			ImGui::TextDisabled("%s", T(TKEY("marker_optimization_unavailable"), "Marker optimization unavailable (PCL not loaded)."));
		}

		ImGui::Checkbox(T(TKEY("use_fps_limit"), "Use FPS Limit"), &settings.reflexUseFPSLimit);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("use_fps_limit_tooltip_1"), "Uses Reflex's internal FPS cap for steadier frametimes."));
			ImGui::TextUnformatted(T(TKEY("use_fps_limit_tooltip_2"), "Can lower latency versus uncapped rendering."));
		}

		if (!settings.reflexLowLatencyMode)
			ImGui::EndDisabled();

		if (!settings.reflexUseFPSLimit)
			ImGui::BeginDisabled();

		if (!std::isfinite(settings.reflexFPSLimit))
			settings.reflexFPSLimit = 60.0f;
		settings.reflexFPSLimit = std::clamp(settings.reflexFPSLimit, 20.0f, 240.0f);
		ImGui::SliderFloat(T(TKEY("fps_limit"), "FPS Limit"), &settings.reflexFPSLimit, 20.0f, 240.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("fps_limit_tooltip_1"), "Set your frame cap target."));
			ImGui::TextUnformatted(T(TKEY("fps_limit_tooltip_2"), "Start about 2-3 FPS below refresh rate (e.g. 117 for 120 Hz)."));
		}

		if (!settings.reflexUseFPSLimit)
			ImGui::EndDisabled();

		if (!reflexAvailable)
			ImGui::EndDisabled();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("backend_diagnostics"), "Backend Diagnostics"))) {
		// Streamline log level selection
		const char* logLevels[] = { "Off", "Default", "Verbose" };
		int logLevelIdx = static_cast<int>(settings.streamlineLogLevel);
		if (ImGui::Combo(T(TKEY("streamline_logging"), "Streamline Logging"), &logLevelIdx, logLevels, IM_ARRAYSIZE(logLevels))) {
			settings.streamlineLogLevel = static_cast<uint>(logLevelIdx);
		}
		ImGui::TextUnformatted(T(TKEY("streamline_logging_restart_note"), "Changing this requires a restart to take effect."));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("streamline_logging_tooltip"), "Streamline logging controls the verbosity of NVIDIA Streamline backend logs. Useful for debugging issues with DLSS."));
		}

		ImGui::Separator();
		Util::DrawDllVersionTable("AMD FidelityFX DLLs (click to open folder)", FidelityFX::PluginDir, FidelityFX::dllVersions, "ffx_dll_versions");
		Util::DrawDllVersionTable("NVIDIA Streamline DLLs (click to open folder)", Streamline::PluginDir, Streamline::dllVersions, "sl_dll_versions");
		ImGui::TreePop();
	}
}

void Upscaling::DrawNeuralRenderingSettings()
{
	auto& nr = settings.neuralRendering;
	bool changed = false;

	if (ImGui::Checkbox(T(TKEY("neural_rendering_enable"), "Enable DLSS 5 Neural Rendering"), &settings.neuralRenderingEnabled))
		changed = true;
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("neural_rendering_enable_tooltip_1"), "DLSS 5 detail synthesis on the render-resolution frame, before DLSS upscales it."));
		ImGui::TextUnformatted(T(TKEY("neural_rendering_enable_tooltip_2"), "Needs nvngx_dlssnr.dll next to sl.interposer.dll in the Streamline folder. Runs on a D3D12 device beside the game's D3D11 one."));
	}

	ImGui::TextWrapped("%s", T(TKEY("nr_intro"), "DLSS 5 Neural Rendering runs NVIDIA's neural detail model over the render-resolution frame, before DLSS upscales it. The model always sees the full picture at render resolution, and DLSS's temporal accumulation then runs over its result."));
	ImGui::TextDisabled("%s: %s", T(TKEY("neural_rendering_status"), "Status"), DlssNR::Describe().c_str());
	if (!DlssNR::FailureReason().empty()) {
		if (ImGui::SmallButton(T(TKEY("neural_rendering_retry"), "Retry")))
			DlssNR::RetryAfterFailure();
	}
	if (dlssBridge.IsQuarantined())
		Util::Text::WrappedWarning("%s", T(TKEY("dlss_bridge_quarantined"), "The D3D12 bridge failed this session; neural rendering is off until restart."));

	ImGui::SeparatorText(T(TKEY("nr_blend_section"), "Blend"));
	uint32_t modelWidth = 0, modelHeight = 0;
	DlssNR::GetModelSize(modelWidth, modelHeight);
	if (modelWidth && modelHeight)
		ImGui::TextDisabled("%s: %ux%u", T(TKEY("nr_model_size"), "Model size"), modelWidth, modelHeight);

	changed |= ImGui::SliderFloat(T(TKEY("nr_detail_strength"), "Detail Strength"), &nr.detailStrength, 0.0f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("nr_detail_strength_tooltip"), "How far the frame moves toward the model's picture. 0 is a true bypass, 1 is the model's picture, above 1 pushes past it."));
	changed |= ImGui::SliderFloat(T(TKEY("nr_colour_strength"), "Colour Strength"), &nr.colourStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("nr_colour_strength_tooltip"), "0 keeps the game's colour and takes only the model's light; 1 takes the model's colour too."));
	changed |= ImGui::SliderFloat(T(TKEY("nr_highlight_guard"), "Highlight Guard"), &nr.maxRatio, 1.0f, 16.0f, "%.1fx");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("nr_highlight_guard_tooltip"), "The most the pass may brighten or darken any pixel."));
	changed |= ImGui::SliderFloat(T(TKEY("nr_motion_scale"), "Motion Scale"), &nr.motionScale, 0.0f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("nr_motion_scale_tooltip"), "Multiplier on the motion vector scale given to the model. 1 assumes normalised screen units. Try 0.5 if moving scenes ghost or smear."));
	bool hdrEncode = nr.hdrEncode != 0;
	if (ImGui::Checkbox(T(TKEY("nr_hdr_encode"), "Tone Map Frame For Model"), &hdrEncode)) {
		nr.hdrEncode = hdrEncode ? 1u : 0u;
		changed = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("nr_hdr_encode_tooltip_1"), "Off (default): the model is handed the raw scene-linear frame at render resolution, as the reference implementation does."));
		ImGui::TextUnformatted(T(TKEY("nr_hdr_encode_tooltip_2"), "On: the frame is tone mapped with a soft knee at the white point below and sRGB-encoded for the model, then decoded back. Try it if sunlit scenes wash out."));
	}
	{
		auto disabled = Util::DisableGuard(!hdrEncode);
		changed |= ImGui::SliderFloat(T(TKEY("nr_hdr_white_point"), "HDR White Point"), &nr.hdrWhitePoint, 0.1f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("nr_hdr_white_point_tooltip"), "Scene value shown to the model as white when tone mapping is on. Raise it if skies look flat, lower it if the model ignores shadow detail."));
	}
	if (DlssNR::IsEncodingHdr())
		ImGui::TextDisabled("%s", T(TKEY("nr_hdr_active"), "Tone mapping the scene-linear frame for the model."));

	ImGui::SeparatorText(T(TKEY("nr_nvidia_section"), "NVIDIA model parameters (rebuild the model when changed)"));

	const char* nrPresets[] = {
		T(TKEY("nr_preset_default"), "Default (model decides)"),
		T(TKEY("nr_preset_1"), "Preset 1"),
		T(TKEY("nr_preset_2"), "Preset 2"),
		T(TKEY("nr_preset_3"), "Preset 3")
	};
	int presetIndex = static_cast<int>(std::min(nr.preset, 3u));
	if (ImGui::Combo(T(TKEY("nr_preset"), "Model Preset"), &presetIndex, nrPresets, IM_ARRAYSIZE(nrPresets))) {
		nr.preset = static_cast<uint32_t>(presetIndex);
		changed = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("nr_preset_tooltip"), "NVIDIA's Neural Rendering model presets. Undocumented; the numbering is not the DLSS super resolution preset scale. Default leaves the choice to the model."));

	const char* nrStyles[] = {
		T(TKEY("nr_style_standard"), "Standard (strongest)"),
		T(TKEY("nr_style_natural"), "Natural"),
		T(TKEY("nr_style_cinematic"), "Cinematic")
	};
	int styleIndex = static_cast<int>(std::min(nr.style, 2u));
	if (ImGui::Combo(T(TKEY("nr_style"), "Style"), &styleIndex, nrStyles, IM_ARRAYSIZE(nrStyles))) {
		nr.style = static_cast<uint32_t>(styleIndex);
		changed = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("nr_style_tooltip_1"), "Standard boosts local contrast and deepens lighting, and can oversaturate or look stylised."));
		ImGui::TextUnformatted(T(TKEY("nr_style_tooltip_2"), "Natural does the same detail work with a gentler hand and keeps skin tones closer to the game. Cinematic tones down the shine for a film-like look."));
	}

	changed |= ImGui::SliderFloat(T(TKEY("nr_intensity"), "Intensity"), &nr.intensity, 0.0f, 4.0f, "%.2f");
	changed |= ImGui::SliderFloat(T(TKEY("nr_local_structure"), "Local Structure"), &nr.localStructureStrength, 0.0f, 4.0f, "%.2f");
	changed |= ImGui::SliderFloat(T(TKEY("nr_local_tone"), "Local Tone"), &nr.localToneStrength, 0.0f, 4.0f, "%.2f");
	changed |= ImGui::SliderFloat(T(TKEY("nr_skin_structure"), "Skin Structure"), &nr.skinStructureStrength, -1.0f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("nr_skin_structure_tooltip"), "Detail the model adds to skin. 1 is the reference default; below 0 follows Local Structure."));
	bool autoMask = nr.useAutoMask != 0;
	if (ImGui::Checkbox(T(TKEY("nr_auto_mask"), "Auto Skin Mask"), &autoMask)) {
		nr.useAutoMask = autoMask ? 1u : 0u;
		changed = true;
	}

	ImGui::SeparatorText(T(TKEY("nr_debug_section"), "Debug"));
	const char* debugViews[] = {
		T(TKEY("nr_debug_off"), "Off"),
		T(TKEY("nr_debug_input"), "Model input"),
		T(TKEY("nr_debug_output"), "Model output"),
		T(TKEY("nr_debug_difference"), "Difference x20")
	};
	int debugIndex = static_cast<int>(std::min(nr.debugView, 3u));
	if (ImGui::Combo(T(TKEY("nr_debug_view"), "Debug View"), &debugIndex, debugViews, IM_ARRAYSIZE(debugViews))) {
		nr.debugView = static_cast<uint32_t>(debugIndex);
		changed = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("nr_debug_view_tooltip"), "A flat grey difference view means the model is doing nothing."));

	if (changed)
		SyncNeuralRenderingSettings();
}

void Upscaling::SyncNeuralRenderingSettings()
{
	DlssNR::SetSettings(settings.neuralRendering);
	DlssNR::SetEnabled(settings.neuralRenderingEnabled);
}

void Upscaling::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Upscaling::WriteGameIniSettings()
{
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->WriteSetting(setting);
		}
	}
}

void Upscaling::LoadSettings(json& o_json)
{
	// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT fills an absent key with
	// Settings' default member initializer (the CURRENT schema version), not 0 --
	// detect absence explicitly so a pre-existing config still runs the migration.
	const bool hadFsr4SchemaVersion = o_json.contains("fsr4RuntimeSelectionSchemaVersion");
	settings = o_json;
	if (!hadFsr4SchemaVersion)
		settings.fsr4RuntimeSelectionSchemaVersion = 0;
	ApplyLegacyFsr4RuntimeSelectionMigration(settings, fidelityFX.GetFsr4AdapterSupport());

	// Sanitize loaded settings to ensure enum indices are valid
	constexpr auto enumCount = 4;  // UpscaleMethod has 4 values: kNONE, kTAA, kFSR, kDLSS
	if (settings.upscaleMethod >= static_cast<uint>(enumCount)) {
		logger::warn("[Upscaling] Loaded upscaleMethod {} out of range, clamping to {}", settings.upscaleMethod, enumCount ? enumCount - 1 : 0);
		settings.upscaleMethod = enumCount ? enumCount - 1 : 0;
	}
	if (settings.upscaleMethodNoDLSS > static_cast<uint>(UpscaleMethod::kFSR)) {
		logger::warn("[Upscaling] Loaded upscaleMethodNoDLSS {} out of range, clamping to {}", settings.upscaleMethodNoDLSS, static_cast<uint>(UpscaleMethod::kFSR));
		settings.upscaleMethodNoDLSS = static_cast<uint>(UpscaleMethod::kFSR);
	}
	if (settings.qualityMode > 4) {
		logger::warn("[Upscaling] Loaded qualityMode {} out of range, resetting to 1", settings.qualityMode);
		settings.qualityMode = 1;
	}
	if (settings.presetDLSS > 4) {
		logger::warn("[Upscaling] Loaded presetDLSS {} out of range, resetting to 0 (Default)", settings.presetDLSS);
		settings.presetDLSS = 0;
	}
	const float originalReflexFPSLimit = settings.reflexFPSLimit;
	if (!std::isfinite(settings.reflexFPSLimit)) {
		settings.reflexFPSLimit = 60.0f;
		logger::warn(
			"[Upscaling] Loaded reflexFPSLimit {} is not finite, resetting to {}",
			originalReflexFPSLimit,
			settings.reflexFPSLimit);
	}
	const float clampedReflexFPSLimit = std::clamp(settings.reflexFPSLimit, 20.0f, 240.0f);
	if (clampedReflexFPSLimit != settings.reflexFPSLimit) {
		logger::warn(
			"[Upscaling] Loaded reflexFPSLimit {} out of range, clamping to {}",
			settings.reflexFPSLimit,
			clampedReflexFPSLimit);
	}
	settings.reflexFPSLimit = clampedReflexFPSLimit;
	if (!std::isfinite(settings.frameGenerationFPSLimit))
		settings.frameGenerationFPSLimit = 120.0f;
	settings.frameGenerationFPSLimit = std::clamp(settings.frameGenerationFPSLimit, 30.0f, 480.0f);
	SyncNeuralRenderingSettings();
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->ReadSetting(setting);
		}
	}
}

void Upscaling::RestoreDefaultSettings()
{
	settings = {};
	SyncNeuralRenderingSettings();
}

void Upscaling::DataLoaded()
{
	// Fix screenshots fix from Engine Fixes
	Util::DisableVanillaTAA();

	// The game defaults this to a non-zero value
	static auto fDRClampOffset = RE::GetINISetting("fDRClampOffset:Display");
	fDRClampOffset->data.f = 0.0f;

	// unk88 keeps the dynamic-resolution viewport, which rounds the 16x1 lens flare visibility target to nothing
	const auto& effects = RE::ImageSpaceManager::GetSingleton()->effects;
	const auto visibilityIndex = RE::ImageSpaceManager::GetCurrentIndex(RE::ImageSpaceManager::ISLensFlareVisibility);
	if (visibilityIndex < effects.size() && effects[visibilityIndex])
		effects[visibilityIndex]->unk88 = false;
}

void Upscaling::Load()
{
	*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChainUpscaling = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChainUpscaling, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
}

struct BSImageSpace_Init_FXAA
{
	static void thunk()
	{
		func();

		// Force FXAA off safely
		auto fxaaEnabled = reinterpret_cast<bool*>(REL::RelocationID(513281, 391028).address());
		*fxaaEnabled = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct FaceGen_CopyRenderTarget
{
	static RE::NiTexture::RendererData* thunk(RE::BSGraphics::Renderer* a_renderer, RE::RENDER_TARGET a_target, D3D11_USAGE a_usage)
	{
		auto source = a_renderer->GetRuntimeData().renderTargets[a_target].texture;
		if (!source) {
			logger::error("[Upscaling] FaceGen texture copy skipped: missing source render target");
			return nullptr;
		}

		// CommonLib's GetSingleton() wrongly dereferences this global: the manager is stored inline, not behind a pointer
		static REL::Relocation<RE::BSGraphics::RenderTargetManager*> renderTargetManager{ REL::RelocationID(524970, 411451) };
		const auto& properties = renderTargetManager->renderTargetData[a_target];
		using CreateTexture = RE::NiTexture::RendererData*(RE::BSGraphics::Renderer*, uint32_t, uint32_t, const void*, D3D11_USAGE, DXGI_FORMAT, bool);
		static REL::Relocation<CreateTexture> createTexture{ REL::RelocationID(75511, 77303) };
		auto texture = createTexture(a_renderer, properties.width, properties.height, nullptr, a_usage, DXGI_FORMAT_R8G8B8A8_UNORM, false);
		if (!texture) {
			logger::error("[Upscaling] FaceGen texture allocation failed ({}x{}, usage {})", properties.width, properties.height, static_cast<uint32_t>(a_usage));
			return nullptr;
		}

		if (!texture->texture || (a_usage != D3D11_USAGE_STAGING && !texture->resourceView)) {
			logger::error("[Upscaling] FaceGen texture allocation returned incomplete resources ({}x{})", properties.width, properties.height);
			if (texture->resourceView)
				texture->resourceView->Release();
			if (texture->texture)
				texture->texture->Release();
			delete texture;
			return nullptr;
		}

		globals::d3d::context->CopyResource(texture->texture, source);
		return texture;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void Upscaling::PostPostLoad()
{
	bool isGOG = !GetModuleHandle(L"steam_api64.dll");
	stl::detour_thunk<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084));

	// Calculates resolution and jitter
	stl::write_thunk_call<Main_UpdateJitter>(REL::RelocationID(75460, 77245).address() + Util::VersionedRelocation::Select(0xE5, isGOG ? 0x133 : 0xE2, 0x133));

	// Disables the original dynamic resolution system
	REL::safe_write(REL::RelocationID(35556, 36555).address() + REL::Relocate(0x2D, 0x2D), REL::NOP5, sizeof(REL::NOP5));

	// Performs upscaling in between volumetric lighting and post processing
	stl::write_thunk_call<Main_PostProcessing>(REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7));

	// Patches facegen texture generation to not use dynamic resolution
	const auto faceGenUpdate = REL::RelocationID(26455, 27041).address();
	const auto faceGenCopy = faceGenUpdate + Util::VersionedRelocation::Select(0x119, 0x187, 0x19E);
	if (Util::VersionedRelocation::IsCallTo(faceGenCopy, REL::RelocationID(75524, 77318).address())) {
		stl::write_thunk_call<FaceGen_CopyRenderTarget>(faceGenCopy);
	} else {
		logger::warn("[Upscaling] FaceGen texture allocation guard not installed: unexpected copy call");
	}
	stl::detour_thunk<BSFaceGenManager_UpdatePendingCustomizationTextures>(faceGenUpdate);

	// Patches precipitation camera to not use dynamic resolution
	stl::write_thunk_call<Main_RenderPrecipitation>(REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x3A1, 0x3A1, 0x3BF));

	// Forces FXAA off
	stl::detour_thunk<BSImageSpace_Init_FXAA>(REL::RelocationID(98974, 105626));

	logger::info("[Upscaling] Installed hooks");
}

#undef I18N_KEY_PREFIX

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod() const
{
	const auto method = (UpscaleMethod)(streamline.featureDLSS ? settings.upscaleMethod : settings.upscaleMethodNoDLSS);
	// FSR3 does not work under RenderDoc
	return method == UpscaleMethod::kFSR && RenderDoc::IsLoaded() ? UpscaleMethod::kTAA : method;
}

void Upscaling::CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Creating texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	if (a_upscalemethod == UpscaleMethod::kDLSS || a_upscalemethod == UpscaleMethod::kFSR) {
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		if (!reactiveMaskTexture) {
			reactiveMaskTexture = new Texture2D(texDesc, "Upscaling::ReactiveMask");
			reactiveMaskTexture->CreateSRV(srvDesc);
			reactiveMaskTexture->CreateUAV(uavDesc);
		}

		if (!transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture = new Texture2D(texDesc, "Upscaling::TransparencyCompositionMask");
			transparencyCompositionMaskTexture->CreateSRV(srvDesc);
			transparencyCompositionMaskTexture->CreateUAV(uavDesc);
		}

		// Cross-API runtime sharing does not support the game's R24G8 depth allocation.
		if (a_upscalemethod == UpscaleMethod::kFSR && fidelityFX.ShouldUseRuntimeUpscalerForFSR() && !runtimeFsrDepthTexture) {
			D3D11_TEXTURE2D_DESC depthDesc{};
			depthDesc.Width = texDesc.Width;
			depthDesc.Height = texDesc.Height;
			depthDesc.MipLevels = 1;
			depthDesc.ArraySize = 1;
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
			depthSrvDesc.Format = depthDesc.Format;
			depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			depthSrvDesc.Texture2D.MipLevels = 1;

			D3D11_UNORDERED_ACCESS_VIEW_DESC depthUavDesc{};
			depthUavDesc.Format = depthDesc.Format;
			depthUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

			runtimeFsrDepthTexture = new Texture2D(depthDesc, "Upscaling::RuntimeFsrDepth");
			runtimeFsrDepthTexture->CreateSRV(depthSrvDesc);
			runtimeFsrDepthTexture->CreateUAV(depthUavDesc);
		}
	}

	// Motion vector copy texture is only needed for DLSS
	if (a_upscalemethod == UpscaleMethod::kDLSS) {
		// Typed R32_FLOAT depth for neural rendering: the pass reads depth across the D3D11/D3D12
		// share, which cannot carry the game's depth-stencil allocation. Created shareable so the
		// bridge opens it in place instead of copying it every frame.
		if (!neuralDepthTexture) {
			D3D11_TEXTURE2D_DESC depthDesc{};
			depthDesc.Width = texDesc.Width;
			depthDesc.Height = texDesc.Height;
			depthDesc.MipLevels = 1;
			depthDesc.ArraySize = 1;
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			depthDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

			D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
			depthSrvDesc.Format = depthDesc.Format;
			depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			depthSrvDesc.Texture2D.MipLevels = 1;

			D3D11_UNORDERED_ACCESS_VIEW_DESC depthUavDesc{};
			depthUavDesc.Format = depthDesc.Format;
			depthUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

			neuralDepthTexture = new Texture2D(depthDesc, "Upscaling::NeuralRenderingDepth");
			neuralDepthTexture->CreateSRV(depthSrvDesc);
			neuralDepthTexture->CreateUAV(depthUavDesc);
		}

		if (!motionVectorCopyTexture) {
			auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

			D3D11_TEXTURE2D_DESC motionTexDesc{};
			motionVector.texture->GetDesc(&motionTexDesc);

			texDesc.Format = motionTexDesc.Format;
			srvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;

			motionVectorCopyTexture = new Texture2D(motionTexDesc, "Upscaling::MotionVectorCopy");
			motionVectorCopyTexture->CreateSRV(srvDesc);
			motionVectorCopyTexture->CreateUAV(uavDesc);
		}

		// RCAS sharpener texture - matches kMAIN format for HDR sharpening
		if (!sharpenerTexture) {
			main.texture->GetDesc(&texDesc);
			main.SRV->GetDesc(&srvDesc);

			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			srvDesc.Format = texDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			uavDesc.Format = texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			sharpenerTexture = new Texture2D(texDesc, "Upscaling::Sharpener");
			sharpenerTexture->CreateSRV(srvDesc);
			sharpenerTexture->CreateUAV(uavDesc);
		}
	}
}

void Upscaling::DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Destroying texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	// Clean up D3D11 textures that are no longer needed
	// Only destroy textures when switching away from methods that use them
	if (a_upscalemethod != UpscaleMethod::kDLSS && a_upscalemethod != UpscaleMethod::kFSR) {
		if (reactiveMaskTexture) {
			reactiveMaskTexture->srv = nullptr;
			reactiveMaskTexture->uav = nullptr;
			reactiveMaskTexture->resource = nullptr;

			delete reactiveMaskTexture;
			reactiveMaskTexture = nullptr;
		}

		if (transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture->srv = nullptr;
			transparencyCompositionMaskTexture->uav = nullptr;
			transparencyCompositionMaskTexture->resource = nullptr;

			delete transparencyCompositionMaskTexture;
			transparencyCompositionMaskTexture = nullptr;
		}
	}

	if (a_upscalemethod != UpscaleMethod::kFSR && runtimeFsrDepthTexture) {
		runtimeFsrDepthTexture->srv = nullptr;
		runtimeFsrDepthTexture->uav = nullptr;
		runtimeFsrDepthTexture->resource = nullptr;

		delete runtimeFsrDepthTexture;
		runtimeFsrDepthTexture = nullptr;
	}

	// Motion vector copy texture is only needed for DLSS - destroy when switching away from DLSS
	if (a_upscalemethod != UpscaleMethod::kDLSS) {
		// The bridge holds D3D12 views of these textures; drop them first.
		dlssBridge.ReleaseSharedResources();
		if (neuralDepthTexture) {
			neuralDepthTexture->srv = nullptr;
			neuralDepthTexture->uav = nullptr;
			neuralDepthTexture->resource = nullptr;

			delete neuralDepthTexture;
			neuralDepthTexture = nullptr;
		}
		if (motionVectorCopyTexture) {
			motionVectorCopyTexture->srv = nullptr;
			motionVectorCopyTexture->uav = nullptr;
			motionVectorCopyTexture->resource = nullptr;

			delete motionVectorCopyTexture;
			motionVectorCopyTexture = nullptr;
		}
		if (sharpenerTexture) {
			sharpenerTexture->srv = nullptr;
			sharpenerTexture->uav = nullptr;
			sharpenerTexture->resource = nullptr;

			delete sharpenerTexture;
			sharpenerTexture = nullptr;
		}
	}
}

void Upscaling::CheckResources(UpscaleMethod a_upscalemethod)
{
	static auto previousUpscaleMode = UpscaleMethod::kTAA;
	static bool previousFrameGenMode = false;

	bool frameGenModeCurrent = (settings.frameGenerationMode && d3d12SwapChainActive);
	bool frameGenModeChanged = frameGenModeCurrent != previousFrameGenMode;
	bool upscaleModeChanged = (previousUpscaleMode != a_upscalemethod);

	if (upscaleModeChanged || frameGenModeChanged) {
		logger::debug("[Upscaling] Resource change detected - Upscale: {} ({}) -> {} ({}), FrameGen: {} -> {} (d3d12Active={})",
			static_cast<int>(previousUpscaleMode), magic_enum::enum_name(previousUpscaleMode), static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod), previousFrameGenMode, frameGenModeCurrent, d3d12SwapChainActive);

		// Destroy previous upscaling method resources (only if they were actually active)
		if (upscaleModeChanged) {
			DestroyUpscalingTextureResources(a_upscalemethod);

			// Only destroy SDK resources if the previous method was actually performing upscaling
			if (previousUpscalingWasActive) {
				if (previousUpscaleMode == UpscaleMethod::kDLSS)
					streamline.DestroyDLSSResources();
				else if (previousUpscaleMode == UpscaleMethod::kFSR)
					fidelityFX.DestroyFSRResources();
			}
			if (a_upscalemethod == UpscaleMethod::kFSR)
				fidelityFX.CreateFSRResources();
		}

		// Create new upscaling method resources
		if (upscaleModeChanged) {
			CreateUpscalingTextureResources(a_upscalemethod);
		}

		// Update tracking for next call
		previousUpscaleMode = a_upscalemethod;
		previousFrameGenMode = (settings.frameGenerationMode && d3d12SwapChainActive);
		previousUpscalingWasActive = a_upscalemethod == UpscaleMethod::kDLSS || a_upscalemethod == UpscaleMethod::kFSR;
	}
}

ID3D11ComputeShader* Upscaling::GetEncodeTexturesCS()
{
	auto upscaleMethod = GetUpscaleMethod();
	uint methodIndex = (uint)upscaleMethod;

	// The runtime FSR provider needs typed R32_FLOAT depth instead of the game's R24G8 resource.
	if (upscaleMethod == UpscaleMethod::kFSR && runtimeFsrDepthTexture) {
		if (!encodeTexturesCSDepthOutput) {
			logger::debug("Compiling EncodeTexturesCS.hlsl for FSR typed depth output");
			std::vector<std::pair<const char*, const char*>> defines = {
				{ "FSR", "" },
				{ "DEPTH_OUTPUT", "" }
			};
			encodeTexturesCSDepthOutput.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0"));
		}
		return encodeTexturesCSDepthOutput.get();
	}

	// Neural rendering needs typed R32_FLOAT depth as well; only compiled and used while it is on.
	if (upscaleMethod == UpscaleMethod::kDLSS && neuralDepthTexture && settings.neuralRenderingEnabled) {
		if (!encodeTexturesCSDepthOutputDLSS) {
			logger::debug("Compiling EncodeTexturesCS.hlsl for DLSS typed depth output");
			std::vector<std::pair<const char*, const char*>> defines = {
				{ "DLSS", "" },
				{ "DEPTH_OUTPUT", "" }
			};
			encodeTexturesCSDepthOutputDLSS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0"));
		}
		if (encodeTexturesCSDepthOutputDLSS)
			return encodeTexturesCSDepthOutputDLSS.get();
	}

	if (!encodeTexturesCS[methodIndex]) {
		logger::debug("Compiling EncodeTexturesCS.hlsl for upscale method {}", methodIndex);

		std::vector<std::pair<const char*, const char*>> defines;

		// Add upscale method define
		switch (upscaleMethod) {
		case UpscaleMethod::kDLSS:
			defines.push_back({ "DLSS", "" });
			break;
		case UpscaleMethod::kFSR:
			defines.push_back({ "FSR", "" });
			break;
		default:
			// No define for NONE or TAA
			break;
		}

		encodeTexturesCS[methodIndex].attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0"));
	}
	return encodeTexturesCS[methodIndex].get();
}

ID3D11PixelShader* Upscaling::GetDepthRefractionUpscalePS()
{
	if (!depthRefractionUpscalePS) {
		logger::debug("Compiling DepthRefractionUpscalePS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
		depthRefractionUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/DepthRefractionUpscalePS.hlsl", defines, "ps_5_0"));
	}

	return depthRefractionUpscalePS.get();
}

ID3D11PixelShader* Upscaling::GetUnderwaterMaskUpscalePS()
{
	if (!underwaterMaskUpscalePS) {
		logger::debug("Compiling UnderwaterMaskPS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
		underwaterMaskUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UnderwaterMaskUpscalePS.hlsl", defines, "ps_5_0"));
	}

	return underwaterMaskUpscalePS.get();
}

ID3D11VertexShader* Upscaling::GetUpscaleVS()
{
	if (!upscaleVS) {
		logger::debug("Compiling UpscaleVS.hlsl");
		upscaleVS.attach((ID3D11VertexShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}

	return upscaleVS.get();
}

eastl::unique_ptr<Texture2D> Upscaling::CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
	bool copyBindFlags, bool createSRV, bool createUAV, const char* name)
{
	D3D11_TEXTURE2D_DESC srcDesc;
	static_cast<ID3D11Texture2D*>(src)->GetDesc(&srcDesc);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = srcDesc.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = copyBindFlags ? srcDesc.BindFlags : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

	auto tex = eastl::make_unique<Texture2D>(desc);

	if (name) {
		Util::SetResourceName(tex->resource.get(), name);
	}

	if (createSRV) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = srcDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		tex->CreateSRV(srvDesc);
	}
	if (createUAV) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = srcDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		tex->CreateUAV(uavDesc);
	}
	return tex;
}

int32_t GetJitterPhaseCount(int32_t renderWidth, int32_t displayWidth)
{
	const float basePhaseCount = 8.0f;
	const int32_t jitterPhaseCount = int32_t(basePhaseCount * pow((float(displayWidth) / renderWidth), 2.0f));
	return jitterPhaseCount;
}

// Calculate halton number for index and base.
static float Halton(int32_t index, int32_t base)
{
	float f = 1.0f, result = 0.0f;

	for (int32_t currentIndex = index; currentIndex > 0;) {
		f /= (float)base;
		result = result + f * (float)(currentIndex % base);
		currentIndex = (uint32_t)(floorf((float)(currentIndex) / (float)(base)));
	}

	return result;
}

void GetJitterOffset(float* outX, float* outY, int32_t index, int32_t phaseCount)
{
	const float x = Halton((index % phaseCount) + 1, 2) - 0.5f;
	const float y = Halton((index % phaseCount) + 1, 3) - 0.5f;

	*outX = x;
	*outY = y;
}

void Upscaling::ConfigureTAA()
{
	auto upscaleMethod = GetUpscaleMethod();

	// Force enable TAA if needed
	Util::SetTemporal(upscaleMethod != UpscaleMethod::kNONE);
}

void Upscaling::ConfigureUpscaling(RE::BSGraphics::State* a_viewport)
{
	auto upscaleMethod = GetUpscaleMethod();

	// Delete or create resources as necessary
	CheckResources(upscaleMethod);

	// Cache original TAA values for UI
	projectionPosScaleX = a_viewport->projectionPosScaleX;
	projectionPosScaleY = a_viewport->projectionPosScaleY;

	// Get full screen size
	auto state = globals::state;
	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };

	auto screenWidth = static_cast<int>(screenSize.x);
	auto screenHeight = static_cast<int>(screenSize.y);

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		float resolutionScaleBase = 1.0f / ffxFsr3GetUpscaleRatioFromQualityMode((FfxFsr3QualityMode)settings.qualityMode);

		auto renderWidth = static_cast<int>(screenWidth * resolutionScaleBase);
		auto renderHeight = static_cast<int>(screenHeight * resolutionScaleBase);

		resolutionScale.x = static_cast<float>(renderWidth) / static_cast<float>(screenWidth);
		resolutionScale.y = static_cast<float>(renderHeight) / static_cast<float>(screenHeight);

		auto phaseCount = GetJitterPhaseCount(renderWidth, screenWidth);

		GetJitterOffset(&jitter.x, &jitter.y, state->frameCount, phaseCount);

		a_viewport->projectionPosScaleX = -2.0f * jitter.x / renderWidth;

		a_viewport->projectionPosScaleY = 2.0f * jitter.y / renderHeight;
	} else {
		resolutionScale = { 1.0f, 1.0f };

		jitter.x = -a_viewport->projectionPosScaleX * screenWidth / 2.0f;

		jitter.y = a_viewport->projectionPosScaleY * screenHeight / 2.0f;
	}

	auto& runtimeData = a_viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = dynamicResolutionWidthRatio;
	runtimeData.dynamicResolutionPreviousHeightRatio = dynamicResolutionHeightRatio;
	runtimeData.dynamicResolutionWidthRatio = resolutionScale.x;
	runtimeData.dynamicResolutionHeightRatio = resolutionScale.y;

	dynamicResolutionWidthRatio = resolutionScale.x;
	dynamicResolutionHeightRatio = resolutionScale.y;

	// Disable dynamic resolution unless the game explicitly enables it
	runtimeData.dynamicResolutionLock = 1;
}

void Upscaling::SetupResources()
{
	QueryPerformanceFrequency(&qpf);

	// Latch the restart-gated runtime-FSR4 opt-in here, not in LoadSettings: LoadSettings can
	// run before the D3D11 device hook, so the adapter probe would report Unsupported and the
	// one-shot RX 7000 migration would not have run yet.
	fsr4RuntimeEnableBoot = settings.fsr4RuntimeEnable;

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
	depthStencilDesc.DepthEnable = true;                           // Enable depth testing
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;  // Write to all depth bits
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;          // Always pass depth test (write all depths)

	depthStencilDesc.StencilEnable = false;  // Disable stencil testing

	DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthStencilDesc, upscaleDepthStencilState.put()));

	// Create jitter offset constant buffer for depth upscaling
	jitterCB = new ConstantBuffer(ConstantBufferDesc<JitterCB>());

	// Create upscaling data constant buffer for encode textures compute shader
	upscalingDataCB = new ConstantBuffer(ConstantBufferDesc<UpscalingDataCB>());

	// Create blend state for depth upscaling
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = false;
	blendDesc.IndependentBlendEnable = false;
	blendDesc.RenderTarget[0].BlendEnable = false;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, upscaleBlendState.put()));

	// Create rasterizer state for fullscreen rendering
	D3D11_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.FrontCounterClockwise = false;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.DepthBiasClamp = 0.0f;
	rasterizerDesc.SlopeScaledDepthBias = 0.0f;
	rasterizerDesc.DepthClipEnable = false;
	rasterizerDesc.ScissorEnable = false;
	rasterizerDesc.MultisampleEnable = false;
	rasterizerDesc.AntialiasedLineEnable = false;
	DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rasterizerDesc, upscaleRasterizerState.put()));

	CheckResources(GetUpscaleMethod());

	rcas.Initialize();

	if (d3d12SwapChainActive)
		dx12SwapChain.CreateSharedResources();

	copyDepthToSharedBufferPS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\Upscaling\\CopyDepthToSharedBufferPS.hlsl", { { "PSHADER", "" } }, "ps_5_0"));
}

void Upscaling::ClearShaderCache()
{
	for (int i = 0; i < 4; ++i) {
		encodeTexturesCS[i] = nullptr;  // com_ptr automatically releases
	}
	encodeTexturesCSDepthOutput = nullptr;  // com_ptr automatically releases
	encodeTexturesCSDepthOutputDLSS = nullptr;

	depthRefractionUpscalePS = nullptr;  // com_ptr automatically releases
	underwaterMaskUpscalePS = nullptr;   // com_ptr automatically releases
	upscaleVS = nullptr;                 // com_ptr automatically releases
}

bool Upscaling::CopySharedD3D12Resources()
{
	// Frame generation must not run on inputs that were never copied this frame.
	auto* vs = GetUpscaleVS();
	auto* ps = copyDepthToSharedBufferPS.get();
	if (!vs || !ps || !dx12SwapChain.motionVectorBufferShared12 || !dx12SwapChain.depthBufferShared12)
		return false;

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Upscaling - Copy Shared D3D12 Resources");
	globals::state->BeginPerfEvent("Copy Shared D3D12 Resources");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	context->CopyResource(dx12SwapChain.motionVectorBufferShared12->resource11, motionVector.texture);

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	{
		// Set up viewport for fullscreen rendering
		float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		// Set up Input Assembler for fullscreen triangle
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Set up vertex shader
		context->VSSetShader(vs, nullptr, 0);

		// Set up rasterizer and blend states
		context->RSSetState(upscaleRasterizerState.get());
		context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

		// Set up pixel shader resources
		ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(views), views);

		// Set render target view for pixel shader output
		ID3D11RenderTargetView* rtvs[1] = { dx12SwapChain.depthBufferShared12->rtv };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(ps, nullptr, 0);

		globals::profiler->BeginPass("Upscaling::CopyDepthD3D12");
		context->Draw(3, 0);
		globals::profiler->EndPass();
	}

	// Clean up
	ID3D11ShaderResourceView* views[1] = { nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(views), views);

	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->PSSetShader(nullptr, nullptr, 0);
	context->VSSetShader(nullptr, nullptr, 0);

	globals::state->EndPerfEvent();
	return true;
}

void UpdateCameraData()
{
	using func_t = decltype(&UpdateCameraData);
	static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };
	func();
}

void Upscaling::PostDisplay()
{
	auto viewport = globals::game::graphicsState;

	viewport->projectionPosScaleX = projectionPosScaleX;
	viewport->projectionPosScaleY = projectionPosScaleY;

	auto& runtimeData = viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = 1;
	runtimeData.dynamicResolutionPreviousHeightRatio = 1;
	runtimeData.dynamicResolutionWidthRatio = 1;
	runtimeData.dynamicResolutionHeightRatio = 1;
	runtimeData.dynamicResolutionLock = 1;

	globals::game::renderer->UpdateViewPort(0, 0, 1);
	UpdateCameraData();

	if (d3d12SwapChainActive)
		globals::features::hdrDisplay.SetUIBuffer();

	globals::state->UpdateSharedData(false, false);
}

void Upscaling::TimerSleepQPC(int64_t targetQPC)
{
	LARGE_INTEGER currentQPC;
	do {
		QueryPerformanceCounter(&currentQPC);
	} while (currentQPC.QuadPart < targetQPC);
}

void Upscaling::FrameLimiter()
{
	if (d3d12SwapChainActive) {
		// Use frame latency waitable object if available for better frame pacing
		HANDLE waitableObject = GetFrameLatencyWaitableObject();

		// Wait for the next frame presentation slot
		static constexpr DWORD kFrameLatencyWaitTimeoutMs = 1000;
		WaitForSingleObject(waitableObject, kFrameLatencyWaitTimeoutMs);

		// The frame generation FPS limit is a second cap on the presented rate that only applies
		// while generation runs; the lower of it and the refresh-rate cap wins.
		const bool generating = ShouldUseFrameGenerationThisFrame();
		const bool fgLimit = generating && settings.frameGenerationFPSLimitEnabled && std::isfinite(settings.frameGenerationFPSLimit);
		if (settings.frameLimitMode || fgLimit) {
			static constexpr int64_t kNanosecondsPerSecond = 1000000000LL;
			static constexpr double kFrameGenerationRateScale = 0.5;
			const double frameRateScale = generating ? kFrameGenerationRateScale : 1.0;
			double presentedCap = settings.frameLimitMode ? refreshRate : 0.0;
			if (fgLimit) {
				const double fgCap = std::clamp(settings.frameGenerationFPSLimit, 30.0f, 480.0f);
				presentedCap = presentedCap > 0.0 ? std::min(presentedCap, fgCap) : fgCap;
			}
			int64_t targetFrameTimeNS = int64_t(static_cast<double>(kNanosecondsPerSecond) / (presentedCap * frameRateScale));
			int64_t targetFrameTicks = (targetFrameTimeNS * qpf.QuadPart) / kNanosecondsPerSecond;

			static LARGE_INTEGER lastFrame = {};
			LARGE_INTEGER timeNow;
			QueryPerformanceCounter(&timeNow);

			int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
			if (delta < targetFrameTicks) {
				TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
			}
			QueryPerformanceCounter(&lastFrame);
		}
	}
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		// using the CCD get the associated path and display configuration
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				// iterate through all the paths until find the exact source to match
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS && wcscmp(info.szDevice, sourceName.viewGdiDeviceName) == 0) {
						// find the matched device which is associated with current device
						// there may be the possibility that display may be duplicated and windows may be one of them in such scenario
						// there may be two callback because source is same target will be different
						// as window is on both the display so either selecting either one is ok
						// get the refresh rate
						UINT numerator = p.targetInfo.refreshRate.Numerator;
						UINT denominator = p.targetInfo.refreshRate.Denominator;
						return (double)numerator / (double)denominator;
					}
				}
			}
		}
	}
	logger::error("Failed to retrieve refresh rate from swap chain");
	return 60;
}

bool Upscaling::IsFrameGenerationDx12PathActive() const
{
	return d3d12SwapChainActive;
}

bool Upscaling::IsFrameGenerationActive() const
{
	return IsFrameGenerationDx12PathActive() && settings.frameGenerationMode && fidelityFX.isFrameGenActive;
}

bool Upscaling::ShouldPrepareFrameGeneration() const
{
	auto* state = globals::state;
	const bool menuOpen = state && state->IsPausedOrMenuOpen(globals::game::ui);
	return IsFrameGenerationDx12PathActive() && settings.frameGenerationMode && (settings.frameGenerationAllowInMenus || !menuOpen);
}

bool Upscaling::ShouldUseFrameGenerationThisFrame() const
{
	return frameGenerationPrepared;
}

bool Upscaling::IsUpscalingActive() const
{
	auto method = GetUpscaleMethod();

	// Only consider vendor upscalers (FSR/DLSS) as "active" when the
	// selected method actually produces a downscale. If the renderer is
	// currently running at 1:1 (no downscale), treat upscaling as inactive.
	if (!(method == UpscaleMethod::kFSR || method == UpscaleMethod::kDLSS)) {
		return false;
	}

	// resolutionScale.x represents renderWidth / displayWidth.
	return resolutionScale.x < .99f;
}

// Unified interface methods
void Upscaling::LoadUpscalingSDKs()
{
	// Initialize upscaling SDK components during plugin startup
	// This ensures all SDKs are available before any D3D device creation
	streamline.LoadInterposer();

	// The neural rendering model and its forwarder sit beside the D3D11 Streamline runtime.
	{
		std::error_code ec;
		auto runtimeDir = std::filesystem::absolute(std::filesystem::path(Streamline::PluginDir), ec);
		DlssNR::SetRuntimeDirectory(ec ? std::wstring(Streamline::PluginDir) : runtimeDir.wstring());
	}

	fidelityFX.LoadFFX();
}

HANDLE Upscaling::GetFrameLatencyWaitableObject() const
{
	return dx12SwapChain.GetFrameLatencyWaitableObject();
}

// Backend interface methods
bool Upscaling::IsBackendInitialized() const
{
	return streamline.initialized;
}

void Upscaling::CheckBackendFeatures(IDXGIAdapter* adapter)
{
	streamline.CheckFeatures(adapter);
}

void Upscaling::UpgradeBackendInterface(void** ppInterface)
{
	streamline.slUpgradeInterface(ppInterface);
}

void Upscaling::SetBackendD3DDevice(ID3D11Device* device)
{
	streamline.slSetD3DDevice(device);
}

void Upscaling::PostBackendDevice()
{
	streamline.PostDevice();
}

// Module availability methods
bool Upscaling::HasFrameGenModule() const
{
	return fidelityFX.featureFSR3FG;
}

void Upscaling::CaptureWorldCamera()
{
	worldCameraFrame = globals::game::frameBufferCached;
	worldCameraFrameValid = true;
}

const globals::FrameBufferCache& Upscaling::GetConstantsCamera() const
{
	return worldCameraFrameValid ? worldCameraFrame : globals::game::frameBufferCached;
}

// Proxy interface methods
void Upscaling::SetProxyD3D11Device(ID3D11Device* device)
{
	dx12SwapChain.SetD3D11Device(device);
}

void Upscaling::SetProxyD3D11DeviceContext(ID3D11DeviceContext* context)
{
	dx12SwapChain.SetD3D11DeviceContext(context);
}

void Upscaling::CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc)
{
	dx12SwapChain.CreateSwapChain(adapter, swapChainDesc);
}

void Upscaling::CreateProxyInterop()
{
	dx12SwapChain.CreateInterop();
}

IDXGISwapChain* Upscaling::GetProxySwapChain()
{
	return dx12SwapChain.GetSwapChainProxy();
}

Upscaling::BlurResources Upscaling::GetBlurResources() const
{
	if (d3d12SwapChainActive) {
		return dx12SwapChain.GetBlurResources();
	}
	return {};
}

void Upscaling::Upscale()
{
	ZoneScoped;
	auto upscaleMethod = GetUpscaleMethod();

	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	{
		globals::profiler->BeginPass("Upscaling::EncodeTextures");
		state->BeginPerfEvent("Encode Upscaling Textures");
		TracyD3D11Zone(globals::state->tracyCtx, "Encode Upscaling Textures");

		auto& temporalAAMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
		auto& normals = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[2]];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		auto renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
		uint32_t renderWidth = (uint32_t)renderSize.x;
		uint32_t renderHeight = (uint32_t)renderSize.y;

		ID3D11ShaderResourceView* views[4] = { temporalAAMask.SRV, normals.SRV, motionVector.SRV, depth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);
		context->CSSetShader(GetEncodeTexturesCS(), nullptr, 0);

		UpscalingDataCB upscalingData;
		upscalingData.trueSamplingDim = float2((float)renderWidth, (float)renderHeight);
		upscalingDataCB->Update(upscalingData);
		auto upscalingBuffer = upscalingDataCB->CB();
		context->CSSetConstantBuffers(0, 1, &upscalingBuffer);

		// u2 (MotionVectorOutput): DLSS only — 5x5 dilated MVec for ghosting reduction.
		// u3 (DepthOutput): runtime FSR only — typed R32_FLOAT copy for the DX11/DX12 bridge.
		// u3 is also the typed depth for DLSS 5 neural rendering while that is on.
		ID3D11UnorderedAccessView* depthOutputUAV = nullptr;
		if (upscaleMethod == UpscaleMethod::kFSR && runtimeFsrDepthTexture)
			depthOutputUAV = runtimeFsrDepthTexture->uav.get();
		else if (upscaleMethod == UpscaleMethod::kDLSS && neuralDepthTexture && settings.neuralRenderingEnabled)
			depthOutputUAV = neuralDepthTexture->uav.get();
		ID3D11UnorderedAccessView* uavs[4] = {
			reactiveMaskTexture->uav.get(),
			transparencyCompositionMaskTexture->uav.get(),
			(upscaleMethod == UpscaleMethod::kDLSS) ? motionVectorCopyTexture->uav.get() : nullptr,
			depthOutputUAV
		};
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);

		ID3D11ShaderResourceView* nullViews[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);

		ID3D11UnorderedAccessView* nullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);

		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);

		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	{
		globals::profiler->BeginPass("Upscaling::Upscale");
		state->BeginPerfEvent("Upscaling");
		TracyD3D11Zone(globals::state->tracyCtx, "Upscaling Dispatch");

		if (upscaleMethod == UpscaleMethod::kDLSS) {
			if (settings.neuralRenderingEnabled && neuralDepthTexture && !dlssBridge.IsQuarantined()) {
				// DLSS 5 neural rendering first, at render resolution on D3D12: the model rewrites the
				// render-resolution rectangle of kMAIN in place, and DLSS then upscales it on D3D11.
				auto renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
				DlssD3D12Bridge::Inputs bridgeInputs{};
				bridgeInputs.dlssInput = main.texture;
				bridgeInputs.depth = neuralDepthTexture->resource.get();
				bridgeInputs.motionVectors = motionVectorCopyTexture->resource.get();
				bridgeInputs.renderWidth = std::max(1u, (uint32_t)renderSize.x);
				bridgeInputs.renderHeight = std::max(1u, (uint32_t)renderSize.y);
				bridgeInputs.resetHistory = state->IsMainOrLoadingMenuOpen();
				// The model wants motion in pixels with the change in jitter since last frame; a gap
				// in the frame sequence means the history does not belong to this frame.
				if (neuralPreviousJitterFrame + 1 == state->frameCount) {
					bridgeInputs.jitterDeltaX = jitter.x - neuralPreviousJitter.x;
					bridgeInputs.jitterDeltaY = jitter.y - neuralPreviousJitter.y;
				} else {
					bridgeInputs.resetHistory = true;
				}
				neuralPreviousJitter = jitter;
				neuralPreviousJitterFrame = state->frameCount;
				dlssBridge.Dispatch(bridgeInputs);
			}
			streamline.Upscale(main.texture,
				settings.dlssHintMasks ? reactiveMaskTexture->resource.get() : nullptr,
				settings.dlssHintMasks ? transparencyCompositionMaskTexture->resource.get() : nullptr,
				motionVectorCopyTexture->resource.get());
		} else if (upscaleMethod == UpscaleMethod::kFSR) {
			auto& depthStencil = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			ID3D11Resource* fsrDepth = runtimeFsrDepthTexture ? runtimeFsrDepthTexture->resource.get() : depthStencil.texture;
			fidelityFX.Upscale(main.texture, fsrDepth, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get(), motionVector.texture, settings.sharpnessFSR);
		}

		state->EndPerfEvent();
		globals::profiler->EndPass();
	}
}

void Upscaling::PerformUpscaling()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Upscaling");
	Upscale();
	UpscaleDepth();

	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();

	// Disable dynamic resolution past this point
	runtimeData.dynamicResolutionLock = 1;

	// Updates the PerFrame constant buffer so that dynamic resolution settings are disabled
	UpdateCameraData();
}

void Upscaling::UpscaleDepth()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Upscaling - Depth");
	// Optimization overview:
	// 1) Early validation exits before issuing GPU work.
	// 2) Wide-kernel depth mode uses hysteresis to avoid frequent toggles.
	// 3) Resource copies are skipped for aliased src/dst to reduce copy churn.

	// (1) Early validation exits
	if (!IsUpscalingActive()) {
		return;
	}

	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!state || !renderer || !context || !deferred || !deferred->linearSampler || !jitterCB || !upscaleRasterizerState || !upscaleBlendState || !upscaleDepthStencilState) {
		return;
	}

	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	if (screenSize.x <= 0.0f || screenSize.y <= 0.0f) {
		return;
	}

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
	auto& refractionNormals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
	auto& saoCameraZ = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSAO_CAMERAZ];
	auto& underwaterMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK];

	if (!depth.texture || !depth.views[0] || !depthCopy.texture || !depthCopy.depthSRV ||
		!refractionNormals.texture || !refractionNormals.textureCopy || !refractionNormals.SRVCopy || !refractionNormals.RTV || !saoCameraZ.RTV ||
		!underwaterMask.texture || !underwaterMask.textureCopy || !underwaterMask.SRVCopy || !underwaterMask.RTV) {
		return;
	}
	auto* fullscreenVS = GetUpscaleVS();
	auto* depthUpscalePS = GetDepthRefractionUpscalePS();
	auto* underwaterMaskPS = GetUnderwaterMaskUpscalePS();
	if (!fullscreenVS || !depthUpscalePS || !underwaterMaskPS) {
		return;
	}

	state->BeginPerfEvent("Render Target Upscaling");

	// Set up Input Assembler for fullscreen triangle (no vertex/index buffers needed)
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Set up vertex shader that generates fullscreen triangle using SV_VertexID
	context->VSSetShader(fullscreenVS, nullptr, 0);

	// Set up viewport for fullscreen rendering
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = screenSize.x;
	viewport.Height = screenSize.y;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	// Set rasterizer and blend state
	context->RSSetState(upscaleRasterizerState.get());
	context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

	ID3D11SamplerState* samplers[] = { deferred->linearSampler };
	context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	// Set up jitter/depth-kernel constant buffer for upscaling
	JitterCB jitterData;
	jitterData.jitter = jitter;
	// (2) Wide-kernel hysteresis
	{
		constexpr float kEnterWideKernelRatio = 1.55f;
		constexpr float kExitWideKernelRatio = 1.45f;
		const float minScale = std::max(std::min(resolutionScale.x, resolutionScale.y), FLT_EPSILON);
		const float upscaleRatio = 1.0f / minScale;

		if (depthUpscaleUseWideKernel) {
			if (upscaleRatio < kExitWideKernelRatio) {
				depthUpscaleUseWideKernel = false;
			}
		} else {
			if (upscaleRatio > kEnterWideKernelRatio) {
				depthUpscaleUseWideKernel = true;
			}
		}

		jitterData.useWideKernel = depthUpscaleUseWideKernel ? 1.0f : 0.0f;
		jitterData.pad0 = 0.0f;
	}

	jitterCB->Update(jitterData);
	auto bufferArray = jitterCB->CB();
	context->PSSetConstantBuffers(0, 1, &bufferArray);

	// (3) Skip aliased copies
	const auto copyIfNonAliased = [&](ID3D11Resource* dst, ID3D11Resource* src) {
		if (dst && src && dst != src) {
			context->CopyResource(dst, src);
		}
	};

	{
		TracyD3D11Zone(globals::state->tracyCtx, "Upscaling - Depth Upscale");

		// Sometimes this is not already copied e.g. map menu.
		// Skip alias copies to reduce unnecessary copy churn.
		copyIfNonAliased(depthCopy.texture, depth.texture);

		// Set depth stencil state to write 0x00
		context->OMSetDepthStencilState(upscaleDepthStencilState.get(), 0x00);

		copyIfNonAliased(refractionNormals.textureCopy, refractionNormals.texture);

		ID3D11ShaderResourceView* srvs[] = { refractionNormals.SRVCopy, depthCopy.depthSRV, depthCopy.stencilSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { refractionNormals.RTV, saoCameraZ.RTV };
		context->OMSetRenderTargets(2, rtvs, depth.views[0]);

		context->PSSetShader(depthUpscalePS, nullptr, 0);
		globals::profiler->BeginPass("Upscaling::DepthUpscale");
		context->Draw(3, 0);
		globals::profiler->EndPass();
	}

	{
		TracyD3D11Zone(globals::state->tracyCtx, "Upscaling - Underwater Mask");

		viewport.Width = screenSize.x * 0.5f;
		viewport.Height = screenSize.y * 0.5f;
		context->RSSetViewports(1, &viewport);

		copyIfNonAliased(underwaterMask.textureCopy, underwaterMask.texture);

		context->OMSetDepthStencilState(nullptr, 0x00);

		// t0: vanilla mask copy, t1: original depth.
		ID3D11ShaderResourceView* srvs[] = { underwaterMask.SRVCopy, depthCopy.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { underwaterMask.RTV };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(underwaterMaskPS, nullptr, 0);
		globals::profiler->BeginPass("Upscaling::UnderwaterMaskUpscale");
		context->Draw(3, 0);
		globals::profiler->EndPass();
	}

	ID3D11ShaderResourceView* nullPSResources[3] = { nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(nullPSResources), nullPSResources);

	state->EndPerfEvent();
}

void Upscaling::ApplySharpening()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Upscaling - Sharpening");

	if (!sharpenerTexture)
		return;

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	if (!main.texture)
		return;

	context->OMSetRenderTargets(0, nullptr, nullptr);

	if (settings.sharpnessEnabledDLSS && settings.sharpnessDLSS > 0.0f && main.UAV) {
		// Match FSR3's slider->RCAS conversion exactly (ffx_fsr3upscaler.cpp + FsrRcasCon):
		//   sharpenessRemapped = -2*slider + 2   (sharpness in stops)
		//   rcasAttenuation    = exp2(-sharpenessRemapped) = exp2(2*slider - 2)
		float currentSharpness = (-2.0f * settings.sharpnessDLSS) + 2.0f;
		currentSharpness = exp2(-currentSharpness);

		// DLSS has already written to sharpenerTexture; sharpen directly into kMAIN.UAV.
		if (!rcas.ApplySharpen(sharpenerTexture->srv.get(), main.UAV, currentSharpness))
			context->CopyResource(main.texture, sharpenerTexture->resource.get());
	} else {
		// Sharpening is disabled: resolve the DLSS output without altering it.
		context->CopyResource(main.texture, sharpenerTexture->resource.get());
	}

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void Upscaling::Main_UpdateJitter::thunk(RE::BSGraphics::State* a_state)
{
	globals::features::upscaling.worldCameraFrameValid = false;
	globals::features::upscaling.ConfigureTAA();
	func(a_state);
	globals::features::upscaling.ConfigureUpscaling(a_state);
}

void Upscaling::MenuManagerDrawInterfaceStartHook::thunk(int64_t a1)
{
	globals::features::upscaling.PostDisplay();

	// For non-Frame Gen HDR: redirect kFRAMEBUFFER.RTV to UI texture before vanilla UI renders
	// When FG is active, its SetUIBuffer redirects to uiBufferWrapped instead
	// When HDR Display is not loaded, skip entirely so vanilla UI renders to kFRAMEBUFFER
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.d3d12SwapChainActive && globals::features::hdrDisplay.loaded) {
		globals::features::hdrDisplay.SetUIBuffer();
	}

	func(a1);
}

void Upscaling::Main_PostProcessing::thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5)
{
	auto& postProcessing = globals::features::postProcessing;
	if (postProcessing.loaded) {
		postProcessing.DrawBeforeUpscaling();
	}

	auto& upscaling = globals::features::upscaling;
	auto upscaleMethod = upscaling.GetUpscaleMethod();

	const bool prepareFrameGeneration = upscaling.ShouldPrepareFrameGeneration();
	if (prepareFrameGeneration && postProcessing.loaded)
		postProcessing.ClearBorderMotionVectorsForFrameGen();
	upscaling.frameGenerationPrepared = prepareFrameGeneration && upscaling.CopySharedD3D12Resources();

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA)
		upscaling.PerformUpscaling();

	if (upscaleMethod == UpscaleMethod::kDLSS)
		upscaling.ApplySharpening();

	Util::SetTemporal(upscaleMethod == UpscaleMethod::kTAA);

	// Redirect kFRAMEBUFFER to float texture before ISHDR runs so HDR values >1.0 survive
	// When HDR Display is not loaded, ISHDR writes to vanilla kFRAMEBUFFER (SDR path)
	bool hdrLoaded = globals::features::hdrDisplay.loaded;
	if (hdrLoaded)
		globals::features::hdrDisplay.RedirectFramebuffer();

	func(a_this, a3, a_target, a_4, a_5);

	// Restore kFRAMEBUFFER after ISHDR — hdrTexture now has the HDR scene
	if (hdrLoaded)
		globals::features::hdrDisplay.RestoreFramebuffer();

	Util::SetTemporal(false);
}

void Upscaling::Main_RenderPrecipitation::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}

void Upscaling::BSFaceGenManager_UpdatePendingCustomizationTextures::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}
