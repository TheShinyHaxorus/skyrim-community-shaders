#ifndef __EYES_HLSLI__
#define __EYES_HLSLI__

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/LightingCommon.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

namespace Eyes
{
	struct CorneaState
	{
		float3 normal;
		float3 F0;
		float roughness;
		float mask;
	};

	CorneaState InitCorneaState(float3 eyeNormal, float3 surfaceNormal, float corneaMask)
	{
		CorneaState state;
		state.normal = normalize(lerp(surfaceNormal, eyeNormal, saturate(SharedData::eyesSettings.CorneaNormalStrength)));
		state.F0 = SharedData::eyesSettings.CorneaF0.xxx;
		state.roughness = clamp(SharedData::eyesSettings.CorneaRoughness, 0.02, 1.0);
		state.mask = saturate(corneaMask);
		return state;
	}

	float2 RefractIrisUV(float2 uv, float3 corneaNormal, float3 viewDirection, float3x3 tbnTr, float corneaMask)
	{
		float depth = SharedData::eyesSettings.IrisDepth * corneaMask;
		if (depth < 1e-5)
			return uv;

		float3 tangentView = normalize(mul(tbnTr, viewDirection));
		float3 tangentNormal = normalize(mul(tbnTr, corneaNormal));

		float eta = rcp(max(SharedData::eyesSettings.IrisIOR, 1.0));
		float3 tangentRefracted = refract(-tangentView, tangentNormal, eta);

		float travel = -tangentRefracted.z;
		if (travel < 1e-3)
			return uv;

		return uv + tangentRefracted.xy * (depth / travel);
	}

	float3 CorneaSpecular(CorneaState state, float3 viewDir, float3 lightDir, float3 halfVector)
	{
		float NdotL = clamp(dot(state.normal, lightDir), EPSILON_DOT_CLAMP, 1);
		float NdotV = saturate(abs(dot(state.normal, viewDir)) + EPSILON_DOT_CLAMP);
		float NdotH = saturate(dot(state.normal, halfVector));
		float VdotH = saturate(dot(viewDir, halfVector));

		float D = BRDF::D_GGX(state.roughness, NdotH);
		float G = BRDF::Vis_SmithJointApprox(state.roughness, NdotV, NdotL);
		float3 F = BRDF::F_Schlick(state.F0, VdotH);

		return D * G * F * NdotL;
	}

	void EvaluateCorneaLighting(CorneaState state, DirectContext context, inout DirectLightingOutput lightingOutput)
	{
		float3 attenuation = 1.0 - BRDF::F_Schlick(state.F0, saturate(dot(state.normal, context.viewDir))) * state.mask;
		lightingOutput.diffuse *= attenuation;
		lightingOutput.specular *= attenuation;

#if defined(TRUE_PBR)
		const float specularScale = 1.0;
#else
		const float specularScale = Color::PBRLightingCompensation * Color::PBRLightingScale;
#endif

		float3 specular = CorneaSpecular(state, context.viewDir, context.lightDir, context.halfVector);
		lightingOutput.specular += specular * context.lightColor * context.detailedShadow * state.mask * specularScale;
	}

	float3 GetCorneaIndirectLobeWeights(inout IndirectLobeWeights lobeWeights, CorneaState state, float3 viewDir)
	{
		float2 specularBRDF = BRDF::EnvBRDF(state.roughness, saturate(dot(state.normal, viewDir)));
		float3 reflectance = (state.F0 * specularBRDF.x + specularBRDF.y) * state.mask;

		float3 attenuation = 1.0 - reflectance;
		lobeWeights.diffuse *= attenuation;
		lobeWeights.specular *= attenuation;
		return reflectance;
	}
}

#endif
