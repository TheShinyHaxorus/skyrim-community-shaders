Texture2D<float4> ScatterPyramid : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float4> SceneColor : register(t2);
RWTexture2D<float4> CompositeTarget : register(u0);

#include "FogScattering/FogScatteringCommon.hlsli"

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= (uint2)BufferDim.xy))
		return;

	int3 coord = int3(dispatchID.xy, 0);
	float4 original = SceneColor.Load(coord);

	float2 uv = ((float2)dispatchID.xy + 0.5f) * BufferDim.zw;
	float weight = FogScattering::GetScatterWeight(uv, SceneDepth.Load(coord));

	float blend = saturate(weight * ScatterParams.x);
	[branch] if (blend <= 0.0f)
	{
		CompositeTarget[dispatchID.xy] = original;
		return;
	}

	float4 gathered = ScatterPyramid.SampleLevel(LinearClampSampler, uv, weight * ScatterParams.y);
	float3 scattered = gathered.rgb / max(gathered.a, 1e-4f);
	scattered = lerp(original.rgb, scattered, saturate(gathered.a));

	CompositeTarget[dispatchID.xy] = float4(lerp(original.rgb, scattered, blend), original.a);
}
