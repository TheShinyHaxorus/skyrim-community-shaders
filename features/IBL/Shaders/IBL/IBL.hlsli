#ifndef __IBL_HLSLI__
#define __IBL_HLSLI__

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

namespace ImageBasedLighting
{
#if defined(IBL_DEFERRED)
	Texture2D<sh2> DiffuseIBLTexture : register(t14);
	Texture2D<sh2> DiffuseSkyIBLTexture : register(t15);
#else
	Texture2D<sh2> DiffuseIBLTexture : register(t76);
	Texture2D<sh2> DiffuseSkyIBLTexture : register(t77);
	TextureCube<float4> StaticDiffuseIBLTexture : register(t78);
	TextureCube<float4> StaticSpecularIBLTexture : register(t79);
#endif

	// Get effective IBL settings based on interior/exterior state
	float GetEffectiveScale() { return SharedData::InInterior ? SharedData::iblSettings.InteriorDiffuseIBLScale : SharedData::iblSettings.DiffuseIBLScale; }
	float GetEffectiveSaturation() { return SharedData::InInterior ? SharedData::iblSettings.InteriorIBLSaturation : SharedData::iblSettings.IBLSaturation; }
	float GetEffectiveDALC() { return SharedData::InInterior ? SharedData::iblSettings.InteriorDALCAmount : SharedData::iblSettings.DALCAmount; }
	float GetInteriorSpecularBoost() { return SharedData::InInterior ? SharedData::iblSettings.InteriorSpecularBoost : 1.0; }

	float3 GetDiffuseIBL(float3 rayDir)
	{
		sh2 shR = DiffuseIBLTexture.Load(int3(0, 0, 0));
		sh2 shG = DiffuseIBLTexture.Load(int3(1, 0, 0));
		sh2 shB = DiffuseIBLTexture.Load(int3(2, 0, 0));
		float colorR = SphericalHarmonics::SHHallucinateZH3Irradiance(shR, rayDir);
		float colorG = SphericalHarmonics::SHHallucinateZH3Irradiance(shG, rayDir);
		float colorB = SphericalHarmonics::SHHallucinateZH3Irradiance(shB, rayDir);
		return float3(colorR, colorG, colorB) / Math::PI;
	}

	float3 GetSkyDiffuseIBL(float3 rayDir)
	{
		sh2 shR = DiffuseSkyIBLTexture.Load(int3(0, 0, 0));
		sh2 shG = DiffuseSkyIBLTexture.Load(int3(1, 0, 0));
		sh2 shB = DiffuseSkyIBLTexture.Load(int3(2, 0, 0));
		float colorR = SphericalHarmonics::SHHallucinateZH3Irradiance(shR, rayDir);
		float colorG = SphericalHarmonics::SHHallucinateZH3Irradiance(shG, rayDir);
		float colorB = SphericalHarmonics::SHHallucinateZH3Irradiance(shB, rayDir);
		return float3(colorR, colorG, colorB) / Math::PI;
	}

#if defined(SKYLIGHTING) && !defined(INTERIOR)
	float3 GetIBLColor(float3 rayDir, float skylighting)
#else
	float3 GetIBLColor(float3 rayDir)
#endif
	{
		float3 color = 0;
		if (SharedData::InInterior)
		{
			color = GetDiffuseIBL(rayDir);
		}
		else
#if defined(SKYLIGHTING)
		{
			color = lerp(GetDiffuseIBL(rayDir), GetSkyDiffuseIBL(rayDir), skylighting);
		}
#else
		{
			color = GetSkyDiffuseIBL(rayDir);
		}
#endif
		return color;
	}

#if defined(LIGHTING)
	float3 GetStaticDiffuseIBL(float3 N, SamplerState samp)
	{
		return StaticDiffuseIBLTexture.SampleLevel(samp, N.xzy, 0).xyz / Math::PI;
	}
#endif

	// Interior light bounce: simulates indirect lighting from surfaces
	// Uses directional ambient as the primary bounce source, modulated by surface normal
	float3 GetInteriorLightBounce(float3 N, float3 directionalAmbientColor)
	{
		float bounceAmount = SharedData::iblSettings.InteriorLightBounce;
		if (bounceAmount <= 0.0)
			return 0;

		// Sample directional ambient from multiple directions to simulate diffuse bounce
		// Hemisphere integration approximation using 6 cardinal directions
		float3 bounceColor = 0;
		bounceColor += max(0, mul(SharedData::DirectionalAmbient, float4(N, 1.0)));
		bounceColor += max(0, mul(SharedData::DirectionalAmbient, float4(-N, 1.0))) * 0.5;  // Back-facing surfaces contribute less
		bounceColor += max(0, mul(SharedData::DirectionalAmbient, float4(float3(N.z, N.y, -N.x), 1.0))) * 0.25;  // Side contributions

		// Normalize and apply bounce factor
		bounceColor = bounceColor / Math::PI;
		return bounceColor * bounceAmount * GetEffectiveSaturation();
	}

	float3 GetFogIBLColor(float3 fogColor)
	{
		float3 directionalAmbientColor = max(0, mul(SharedData::DirectionalAmbient, float4(float3(0, 0, 0), 1.0))).xyz;
		float3 iblColor = directionalAmbientColor * SharedData::iblSettings.DALCAmount + Color::Saturation(GetSkyDiffuseIBL(float3(0, 0, 0)), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
		if (SharedData::iblSettings.PreserveFogLuminance) {
			const float fogLuminance = Color::RGBToLuminance(fogColor);
			const float iblLuminance = Color::RGBToLuminance(iblColor);
			if (iblLuminance > 0) {
				const float scale = fogLuminance / iblLuminance;
				iblColor *= scale;
			} else {
				iblColor = fogColor;
			}
		}
		return lerp(fogColor, iblColor, SharedData::iblSettings.FogAmount);
	}
}

#endif // __IBL_HLSLI__