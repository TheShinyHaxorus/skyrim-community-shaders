Texture2D<float4> SourceMip : register(t0);
RWTexture2D<float4> TargetMip : register(u0);

#include "FogScattering/FogScatteringCommon.hlsli"

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint targetWidth, targetHeight;
	TargetMip.GetDimensions(targetWidth, targetHeight);
	if (any(dispatchID.xy >= uint2(targetWidth, targetHeight)))
		return;

	uint sourceWidth, sourceHeight;
	SourceMip.GetDimensions(sourceWidth, sourceHeight);
	float2 sourceTexel = 1.0f / float2(sourceWidth, sourceHeight);

	float2 uv = ((float2)dispatchID.xy + 0.5f) / float2(targetWidth, targetHeight);

	float4 sum = SourceMip.SampleLevel(LinearClampSampler, uv + sourceTexel * float2(-1.0f, -1.0f), 0);
	sum += SourceMip.SampleLevel(LinearClampSampler, uv + sourceTexel * float2(1.0f, -1.0f), 0);
	sum += SourceMip.SampleLevel(LinearClampSampler, uv + sourceTexel * float2(-1.0f, 1.0f), 0);
	sum += SourceMip.SampleLevel(LinearClampSampler, uv + sourceTexel * float2(1.0f, 1.0f), 0);

	TargetMip[dispatchID.xy] = sum * 0.25f;
}
