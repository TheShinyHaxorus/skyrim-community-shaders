#ifndef __EYES_HLSLI__
#define __EYES_HLSLI__

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/LightingCommon.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/SpecularAA.hlsli"

namespace Eyes
{
	struct Cornea
	{
		float3 normal;
		float3 F0;
		float roughness;
		float wideRoughness;
		float energy;
		float coverage;
	};

	float GetUVPerRadian(float2 uv, float3 eyeballNormal)
	{
		float2 duvdx = ddx(uv);
		float2 duvdy = ddy(uv);
		float3 dndx = ddx(eyeballNormal);
		float3 dndy = ddy(eyeballNormal);

		float uvSq = dot(duvdx, duvdx) + dot(duvdy, duvdy);
		float radianSq = dot(dndx, dndx) + dot(dndy, dndy);

		return clamp(sqrt(uvSq / max(radianSq, 1e-12)), 0.0, 1.0);
	}

	float GetCorneaCoverage(float envMask, bool hasEnvMask)
	{
		if (!hasEnvMask)
			return 1.0;
		return lerp(1.0, saturate(envMask), saturate(SharedData::eyesSettings.CorneaFromEnvMask));
	}

	float2 RefractIris(float2 uv, float3 eyeballNormal, float3 viewDirection, float3x3 tbnTr, float uvPerRadian, float coverage)
	{
		float depth = SharedData::eyesSettings.AnteriorChamberDepth * coverage * uvPerRadian;
		if (depth < 1e-6)
			return uv;

		float3 refracted = refract(-viewDirection, eyeballNormal, rcp(max(SharedData::eyesSettings.AqueousIOR, 1.0)));
		float3 refractedTS = mul(tbnTr, refracted);

		float axial = -refractedTS.z;
		if (axial < 1e-3)
			return uv;

		float2 offset = refractedTS.xy * (depth / axial);
		return uv + clamp(offset, -0.25, 0.25);
	}

	Cornea InitCornea(float3 eyeballNormal, float3 surfaceNormal, float coverage)
	{
		Cornea cornea;

		cornea.coverage = saturate(coverage);
		cornea.normal = normalize(lerp(surfaceNormal, eyeballNormal, saturate(SharedData::eyesSettings.CorneaSmoothing)));
		cornea.F0 = SharedData::eyesSettings.CorneaF0.xxx;

		float perceptualRoughness = lerp(SharedData::eyesSettings.ScleraRoughness, SharedData::eyesSettings.CorneaRoughness, cornea.coverage);
		cornea.roughness = SpecularAA::FilterRoughness(clamp(perceptualRoughness, 0.02, 1.0), cornea.normal);

		float alpha = cornea.roughness * cornea.roughness;
		float alphaPrime = saturate(alpha + SharedData::eyesSettings.CatchlightSize);
		float widening = alpha / max(alphaPrime, EPSILON_DIVISION);

		cornea.wideRoughness = sqrt(alphaPrime);
		cornea.energy = widening * widening;
		return cornea;
	}

	float3 CorneaSpecular(Cornea cornea, float3 viewDir, float3 lightDir, float3 halfVector)
	{
		float NdotL = clamp(dot(cornea.normal, lightDir), EPSILON_DOT_CLAMP, 1);
		float NdotV = saturate(abs(dot(cornea.normal, viewDir)) + EPSILON_DOT_CLAMP);
		float NdotH = saturate(dot(cornea.normal, halfVector));
		float VdotH = saturate(dot(viewDir, halfVector));

		float D = BRDF::D_GGX(cornea.wideRoughness, NdotH) * cornea.energy;
		float G = BRDF::Vis_SmithJointApprox(cornea.wideRoughness, NdotV, NdotL);
		float3 F = BRDF::F_Schlick(cornea.F0, VdotH);

		return D * G * F * NdotL * SharedData::eyesSettings.CatchlightStrength;
	}

	void EvaluateCorneaLighting(Cornea cornea, DirectContext context, inout DirectLightingOutput lightingOutput)
	{
		float3 attenuation = 1.0 - saturate(BRDF::F_Schlick(cornea.F0, saturate(dot(cornea.normal, context.viewDir))));
		lightingOutput.diffuse *= attenuation;
		lightingOutput.specular *= attenuation * (1.0 - cornea.coverage);

#if defined(TRUE_PBR)
		const float specularScale = 1.0;
#else
		const float specularScale = Color::PBRLightingCompensation * Color::PBRLightingScale;
#endif

		float3 specular = CorneaSpecular(cornea, context.viewDir, context.lightDir, context.halfVector);
		lightingOutput.specular += specular * context.lightColor * context.detailedShadow * specularScale;
	}

	float3 GetCorneaIndirectLobeWeights(inout IndirectLobeWeights lobeWeights, Cornea cornea, float3 viewDir)
	{
		float2 specularBRDF = BRDF::EnvBRDF(cornea.roughness, saturate(dot(cornea.normal, viewDir)));
		float3 reflectance = saturate(cornea.F0 * specularBRDF.x + specularBRDF.y);

		lobeWeights.diffuse *= 1.0 - reflectance;

		return reflectance * SharedData::eyesSettings.ReflectionStrength;
	}
}

#endif
