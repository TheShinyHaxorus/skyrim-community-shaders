#pragma once

#include "Buffer.h"
#include "Feature.h"

/**
 * @brief Approximates multiple scattering in fog by blending the scene toward a
 * fog-weighted blur of itself, so surfaces lose detail as they dissolve into haze
 * instead of staying razor sharp behind it.
 *
 * Runs entirely in screen space at the pre-tonemap hook, so it is independent of
 * which fog is actually producing the haze - Exponential Height Fog, vanilla, or an
 * ENB preset driving Effects11. Density is read from the engine's own fog curve
 * (RE::Sky), with manual overrides for setups that replace it.
 *
 * The blur is a weighted mip pyramid rather than a per-pixel variable-radius kernel:
 * the filter is deterministic, the radius varies smoothly across mips, and no tap
 * weight is unbounded, which is what keeps it stable under TAA jitter.
 */
struct FogScattering : Feature
{
	virtual inline std::string GetName() override { return "Fog Scattering"; }
	virtual std::string GetDisplayName() override { return T("feature.fog_scattering.name", "Fog Scattering"); }
	virtual inline std::string GetShortName() override { return "FogScattering"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool HasShaderDefine(RE::BSShader::Type) override { return false; }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fog_scattering.description", "Single-scattering fog attenuates a surface and tints it, but leaves it perfectly sharp. Real fog scatters light many times before it reaches the eye, which softens whatever is behind it. This pass approximates that by blending the scene toward a blur of itself, weighted by how much fog sits in front of each pixel."),
			{ T("feature.fog_scattering.key_feature_1", "Distant geometry dissolves into haze instead of staying sharp behind it"),
				T("feature.fog_scattering.key_feature_2", "Reads the engine's own fog curve, so it works with vanilla fog, Exponential Height Fog, or an ENB preset"),
				T("feature.fog_scattering.key_feature_3", "Half-resolution weighted mip pyramid; one prepare, a short downsample chain and one composite") } };
	};

	struct Settings
	{
		bool Enable = true;
		float Intensity = 0.7f;
		float Radius = 0.6f;
		float DensityThreshold = 0.15f;
		float MaxLuminance = 8.0f;
		bool UseManualFog = false;
		float ManualFogNear = 2048.0f;
		float ManualFogFar = 40960.0f;
		float ManualFogPower = 1.0f;
		float ManualFogClamp = 1.0f;
	};

	Settings settings;

	/** @brief Outcome of the most recent frame, surfaced in the UI so a silent skip is diagnosable. */
	enum class PassStatus
	{
		NotRun,
		Applied,
		Disabled,
		MenuOpen,
		NoFog,
		NoRenderer,
		NoSceneTarget,
		NoDepth,
		NoResources,
		NoShaders,
		BadProjection,
	};

	/** @brief Renders the mask that drives the blend instead of the scattered scene. Not persisted. */
	bool debugShowMask = false;

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	/**
	 * @brief Applies the scattering pass in place on the supplied HDR scene colour target.
	 * @param a_input Render target the engine is about to tonemap from.
	 */
	void ApplyScattering(RE::RENDER_TARGET a_input);

private:
	struct alignas(16) FogScatteringCB
	{
		float4 BufferDim;
		float4 ScatterDim;
		float4 CameraData;
		float4 FogParams;
		float4 ViewRayParams;
		float4 ScatterParams;
		float4 ScatterParams2;
	};
	STATIC_ASSERT_ALIGNAS_16(FogScatteringCB);

	void EnsureResources(uint32_t a_width, uint32_t a_height, ID3D11Texture2D* a_sceneTexture);
	void ReleaseResources();

	/**
	 * @brief Recompiles the depth-sampling shaders when the bound depth SRV changes type class.
	 *
	 * Terrain Blending swaps the scene depth SRV between R32_FLOAT and the game's
	 * R24_UNORM_X8_TYPELESS. A `Texture2D<float>` read of a UNORM view is an invalid
	 * resource return type, so the variant has to follow the view actually bound.
	 */
	void EnsureShaderVariant(ID3D11ShaderResourceView* a_depthSRV);
	ID3D11ComputeShader* GetPrepareCS();
	ID3D11ComputeShader* GetDownsampleCS();
	ID3D11ComputeShader* GetCompositeCS();

	void SetStatus(PassStatus a_status);

	/**
	 * @brief Resolves the fog curve to feed the density estimate.
	 * @param o_fogParams Receives (near, 1/range, power, clamp).
	 * @return False when no meaningful fog is active, in which case the pass is skipped.
	 */
	bool ResolveFogParams(float4& o_fogParams) const;

	std::unique_ptr<Texture2D> scatterPyramid;
	std::unique_ptr<Texture2D> compositeTarget;
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> mipSRVs;
	std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>> mipUAVs;
	std::unique_ptr<ConstantBuffer> fogScatteringCB;
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;

	ID3D11ComputeShader* prepareCS = nullptr;
	ID3D11ComputeShader* downsampleCS = nullptr;
	ID3D11ComputeShader* compositeCS = nullptr;

	uint32_t scatterWidth = 0;
	uint32_t scatterHeight = 0;
	uint32_t mipCount = 0;
	uint32_t lastAppliedFrame = UINT32_MAX;
	float4 resolvedFogParams{};

	uint32_t sceneWidth = 0;
	uint32_t sceneHeight = 0;
	DXGI_FORMAT sceneFormat = DXGI_FORMAT_UNKNOWN;

	bool depthIsUnorm = false;
	bool shaderVariantResolved = false;

	PassStatus lastStatus = PassStatus::NotRun;
	RE::RENDER_TARGET lastInputTarget = RE::RENDER_TARGET::kNONE;
};
