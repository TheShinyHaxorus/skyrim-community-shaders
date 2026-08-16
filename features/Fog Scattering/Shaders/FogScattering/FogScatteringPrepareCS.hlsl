Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);
RWTexture2D<float4> ScatterTarget : register(u0);

#include "FogScattering/FogScatteringCommon.hlsli"

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= (uint2)ScatterDim.xy))
		return;

	float2 uv = ((float2)dispatchID.xy + 0.5f) * ScatterDim.zw;
	int3 fullCoord = int3(min(uv * BufferDim.xy, BufferDim.xy - 1.0f), 0);

	float weight = FogScattering::GetScatterWeight(uv, SceneDepth.Load(fullCoord));
	float3 color = FogScattering::ClampLuminance(max(SceneColor.Load(fullCoord).rgb, 0.0f));

	ScatterTarget[dispatchID.xy] = float4(color * weight, weight);
}
