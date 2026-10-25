#pragma once

// FSR 4 from AMD's driver library (amdxcffx64.dll) on GPUs whose driver has none.
//
// The FFX loader only reaches FSR 4 through the AMD driver, so on NVIDIA and Intel the runtime
// upscaler falls back to FSR 3.1. The driver library carries the FSR 4 providers itself (FP8, INT8,
// INT8 for RDNA 2 and INT8 Lite, version 4.1.2 in the 2.4.0 build) and hands them out through
// UpdateFfxApiProvider. This module loads it from the FidelityFX folder, lets it pass its checks
// for the signed AMD SDK libraries, steers its model choice to an INT8 model, and wraps the FFX
// function table so contexts created for its provider are served by it. Ported from OptiScaler's
// FSR4DriverProvider, FSR4DriverProviderDx12, FSR4Models and rtti scanner (GPL-3).

#include <FidelityFX/api/include/ffx_api.h>
#include <FidelityFX/api/include/ffx_api_loader.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace DriverFsr4
{
	enum class Model : uint32_t
	{
		Int8 = 0,      ///< The RDNA 3 INT8 network; the best quality an INT8 GPU can run
		Int8Lite = 1,  ///< The smaller INT8 network for APUs; standard shader features only
	};

	/// Loads the driver library from a_dllPath and gets its upscale provider. Call once, after the
	/// FFX loader and upscaler libraries are loaded (the driver library checks their signatures).
	bool Load(const std::filesystem::path& a_dllPath, Model a_model);

	/// Routes calls for driver-provider contexts through a_module, and lists the driver provider
	/// first in upscale version queries so a version override can select it.
	void InstallShims(ffxFunctions& a_module);

	bool IsAvailable();
	uint64_t VersionId();
	const std::string& VersionName();
	const char* ModelName(Model a_model);
	/// The model the provider ran its last support checks for; nullptr before any context exists.
	const char* ActiveModelName();
	/// Why loading failed, empty when it worked or was not attempted.
	const std::string& FailureReason();
}
