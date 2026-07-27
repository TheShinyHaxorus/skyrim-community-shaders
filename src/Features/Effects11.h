#pragma once

#include "Buffer.h"

#include <memory>
#include <winrt/base.h>

struct Effects11 : Feature
{
public:
	virtual inline std::string GetName() override { return "Effects11"; }
	virtual inline std::string GetShortName() override { return "Effects11"; }
	virtual inline std::string GetDisplayName() override { return "Effects 11"; }
	virtual std::string_view GetCategory() const override { return "Post-Processing"; }
	virtual inline std::string_view GetShaderDefineName() override { return "EFFECTS11"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.effects11.description", "Effects 11 provides a framework for loading and executing ENBSeries-compatible FX effect files.\nThis allows for advanced post-processing effects and visual enhancements using DirectX 11 Effect (.fx) files."),
			{ T("feature.effects11.key_feature_1", "ENBSeries-compatible FX support"),
				T("feature.effects11.key_feature_2", "DirectX 11 Effect file loading"),
				T("feature.effects11.key_feature_3", "Advanced post-processing pipeline"),
				T("feature.effects11.key_feature_4", "Custom technique execution"),
				T("feature.effects11.key_feature_5", "Dynamic UI variable system") }
		};
	}

	struct alignas(16) PerFrame
	{
		uint Enable;
		uint EnableSky;
		float ColorPow;
		float LightSpriteIntensity;

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		float VolumetricRaysDesaturation;
		float3 VolumetricRaysColorFilter;

		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;
		uint EnableProceduralSun;
		float ProceduralSunDiskRadiusSq;

		float ProceduralSunDiskEdgeScale;
		float ProceduralSunGlowIntensity;
		float ProceduralSunCoronaFalloff;
		float ProceduralSunCoronaScale;

		float ParticleIntensity;
		float ParticleLightingInfluence;
		float ParticleAmbientInfluence;
		float ParticlePointLightingInfluence;

		uint EnableCloudsScattering;
		uint EnableCloudsLightingFromMoon;
		float SkyScatteringIntensity;
		float SkyScatteringAmount;

		float3 SkyScatteringColor;
		float SkyScatteringColorFromSun;

		float SkyScatteringCloudsLightingSunMultiplier;
		float SkyScatteringCloudsLightingMoonIntensity;
		float SkyScatteringPadding;

		uint EnableVolumetricRays;
		float VolumetricRaysIntensity;
		float VolumetricRaysExtinction;
		float VolumetricRaysSkyColorAmount;

		uint EnableRain;
		float RainMotionStretch;
		float RainMotionTransparency;
		float RainLightingInfluence;

		float FireIntensity;
		float FireCurve;
		uint EnableWater;
		float WaterWavesAmplitude;

		float WaterMuddiness;
		float WaterSunLightingMultiplier;
		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;

		float WaterFresnelMax;
		float WaterFresnelMultiplier;
		float WaterReflectionAmount;
		float2 EnbPadding;
	};

	bool enableEffect = false;
	float4 vanillaTonemapParams[7]{};
	bool vanillaTonemapParamsValid = false;
	winrt::com_ptr<ID3D11Buffer> tonemapParamsStaging[2];
	bool tonemapParamsStagingHasData[2]{};
	uint32_t tonemapParamsSwap = 0;
	void CaptureVanillaTonemapParams();

	ID3D11PixelShader* raymarchVolumetricRaysPS = nullptr;
	ID3D11PixelShader* applyVolumetricRaysPS = nullptr;
	ID3D11ComputeShader* blurHCS = nullptr;
	ID3D11ComputeShader* blurVCS = nullptr;
	winrt::com_ptr<ID3D11BlendState> additiveBlendState;
	winrt::com_ptr<ID3D11BlendState> alphaBlendState;

	std::unique_ptr<Texture2D> vlTexA;
	std::unique_ptr<Texture2D> vlTexB;
	// Depth each half-res texel raymarched with; the bilateral blur + upsample weight against it.
	std::unique_ptr<Texture2D> vlDepthHalf;
	std::unique_ptr<ConstantBuffer> vlBlurCB;

	winrt::com_ptr<ID3D11Texture2D> raindropTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> raindropSRV;
	std::string raindropStatus;
	void LoadRaindropTexture();

	PerFrame GetCommonBufferData();

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void ClearShaderCache() override;

	void DrawVolumetricRays();

	void OnSkyUpdateColors(RE::Sky* a_sky);
	void OverrideWeather(RE::Sky* a_sky);
	void CheckCommonData();
	void OverridePointLightColor(float3& a_color);

	struct DirectionalAmbientColors
	{
		RE::NiColor directionalAmbientColors[3][2];
	};
	void OverrideAmbientLighting(DirectionalAmbientColors& DirectionalAmbientColors);

	void ModifySky(RE::BSRenderPass* Pass);
	__declspec(noinline) void ModifyParticle(RE::BSRenderPass* Pass);
	void ParticleShaderHacks();
	bool HandleTonemapRender(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output);
};
