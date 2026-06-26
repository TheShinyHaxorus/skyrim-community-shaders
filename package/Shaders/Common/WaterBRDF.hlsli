#ifndef WATER_BRDF_HLSLI
#define WATER_BRDF_HLSLI

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"

// Cook-Torrance microfacet sun specular for water (GGX + Smith-GGX + Schlick).
// Maths from Google Filament: https://google.github.io/filament/Filament.md.html
namespace WaterBRDF
{
	// IOR 1.33 -> F0 = ((1.33-1)/(1.33+1))^2 = 0.02 (achromatic dielectric).
	static const float3 WATER_F0 = float3(0.02f, 0.02f, 0.02f);

	// Artistic boost: real water reads brighter than a single-layer BRDF predicts.
	static const float WATER_SPECULAR_ENERGY_BOOST = 2.5f;

	// Blinn-Phong specular power -> GGX roughness (alpha).
	float SpecularPowerToRoughness(float specPower)
	{
		float perceptualRoughness = saturate(sqrt(2.0f / (specPower + 2.0f)));
		perceptualRoughness = max(perceptualRoughness, 0.001f);
		return perceptualRoughness * perceptualRoughness;
	}

	// GGX normal distribution (Filament's numerically-stable form).
	float D_GGX(float NdotH, float roughness)
	{
		float a = NdotH * roughness;
		float k = roughness / (1.0f - NdotH * NdotH + a * a);
		return k * k * (1.0f / Math::PI);
	}

	// Smith-GGX height-correlated visibility (includes the 4*NdotV*NdotL denom).
	float V_SmithGGXCorrelated(float NdotV, float NdotL, float roughness)
	{
		float GGXV = NdotL * (NdotV * (1.0f - roughness) + roughness);
		float GGXL = NdotV * (NdotL * (1.0f - roughness) + roughness);
		return 0.5f / max(GGXV + GGXL, 1e-6f);
	}

	// Schlick Fresnel with F90 = 1 (mirror-like at grazing angles).
	float3 F_Schlick(float VdotH, float3 f0)
	{
		float f = pow(1.0f - VdotH, 5.0f);
		return f + f0 * (1.0f - f);
	}

	// D * Vis * F. viewDirection is camera->surface; lightDirection points toward the sun.
	float3 SpecularBRDF(float3 normal, float3 viewDirection, float3 lightDirection, float specPower)
	{
		float3 V = -viewDirection;
		float3 L = lightDirection;
		float3 H = normalize(L + V);

		float NdotH = saturate(dot(normal, H));
		float NdotV = max(dot(normal, V), 1e-4f);
		float NdotL = saturate(dot(normal, L));
		float VdotH = saturate(dot(V, H));

		if (NdotL < 1e-4f)
			return 0.0.xxx;

		float roughness = SpecularPowerToRoughness(specPower);
		float D = D_GGX(NdotH, roughness);
		float Vis = V_SmithGGXCorrelated(NdotV, NdotL, roughness);
		float3 F = F_Schlick(VdotH, WATER_F0);
		return D * Vis * F;
	}

	// Barré-Brisebois backlit translucency. Thickness proxy is normal tilt (wavelet
	// faces) since flat normal-mapped water has no Gerstner crest height.
	// https://www.alanzucconi.com/2017/08/30/fast-subsurface-scattering-1/
	static const float SSS_DISTORTION = 0.2f;   // normal deflection of the back-light
	static const float SSS_POWER = 8.0f;        // back-scatter lobe sharpness
	static const float SSS_SCALE = 1.5f;        // back-scatter brightness
	static const float SSS_RIM_POWER = 4.0f;    // grazing rim sharpness
	static const float SSS_RIM_SCALE = 0.5f;    // grazing rim brightness
	static const float SSS_STRENGTH = 1.0f;     // overall master multiplier

	// viewDirection is camera->surface; sunDir points toward the sun; shallowColor is ShallowColor.xyz.
	float3 GetBacklitSSS(float3 normal, float3 viewDirection, float3 sunDir, float3 sunColor, float3 shallowColor)
	{
		float nlen = length(normal);
		float3 N = (nlen > 1e-4f) ? (normal / nlen) : float3(0.0f, 0.0f, 1.0f);
		float3 V = -viewDirection;
		float3 L = sunDir;

		// Only backlit: viewer and sun on opposite sides of the surface.
		float sunBehind = saturate(dot(-L, V));
		if (sunBehind <= 0.0f)
			return 0.0.xxx;

		// Micro-crest proxy: normals tilted off world-up read as wavelet faces.
		float thickness = 1.0f - saturate(dot(N, float3(0.0f, 0.0f, 1.0f)));

		// Term 1: back-scatter lobe.
		float3 H_back = normalize(-L + N * SSS_DISTORTION);
		float backScatter = pow(saturate(dot(V, -H_back)), SSS_POWER) * SSS_SCALE;

		// Term 2: grazing rim glow, peaked toward silhouette (back-facing) normals.
		float NdotV = saturate(dot(N, V));
		float rim = pow(1.0f - NdotV, SSS_RIM_POWER) * (1.0f - NdotV) * SSS_RIM_SCALE;

		float3 tint = Color::Water(shallowColor) * sunColor;
		return tint * (backScatter + rim) * thickness * sunBehind * SSS_STRENGTH;
	}
}

#endif  // WATER_BRDF_HLSLI
