#ifndef __FOG_SCATTERING_COMMON_HLSLI__
#define __FOG_SCATTERING_COMMON_HLSLI__

cbuffer FogScatteringCB : register(b0)
{
	float4 BufferDim;
	float4 ScatterDim;
	float4 CameraData;
	float4 FogParams;
	float4 ViewRayParams;
	float4 ScatterParams;
	float4 ScatterParams2;
};

SamplerState LinearClampSampler : register(s0);

namespace FogScattering
{
	float GetLinearDepth(float rawDepth)
	{
		return CameraData.w / (-rawDepth * CameraData.z + CameraData.x);
	}

	float GetViewDistance(float2 uv, float linearDepth)
	{
		float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
		float3 viewRay = float3(ndc * ViewRayParams.xy, 1.0);
		return linearDepth * length(viewRay);
	}

	float GetFogAlpha(float viewDistance)
	{
		float normalized = saturate((viewDistance - FogParams.x) * FogParams.y);
		return min(FogParams.w, exp2(FogParams.z * log2(max(normalized, 1e-6))));
	}

	// Scattering weight rises smoothly from the density threshold to 1. Every term is
	// bounded and monotonic so no weight can latch between frames under TAA jitter.
	float GetScatterWeight(float2 uv, float rawDepth)
	{
		float linearDepth = GetLinearDepth(rawDepth);
		if (linearDepth >= CameraData.x * 0.9)
			return 0.0;

		float fogAlpha = GetFogAlpha(GetViewDistance(uv, linearDepth));
		float threshold = ScatterParams.z;
		return saturate((fogAlpha - threshold) / max(1.0 - threshold, 1e-4));
	}

	float3 ClampLuminance(float3 color)
	{
		float luminance = max(dot(color, float3(0.2126, 0.7152, 0.0722)), 1e-5);
		return color * min(1.0, ScatterParams2.x / luminance);
	}
}

#endif
