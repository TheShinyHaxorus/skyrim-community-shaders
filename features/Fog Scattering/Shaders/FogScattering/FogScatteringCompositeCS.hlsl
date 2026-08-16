Texture2D<float4> ScatterPyramid : register(t0);
// DEPTH_UNORM tracks the depth SRV the C++ side binds: R24_UNORM_X8_TYPELESS game depth
// (unorm) without Terrain Blending, R32_FLOAT blended depth with it.
#if defined(DEPTH_UNORM)
Texture2D<unorm float> SceneDepth : register(t1);
#else
Texture2D<float> SceneDepth : register(t1);
#endif
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

	[branch] if (ScatterParams2.y > 0.5f)
	{
		CompositeTarget[dispatchID.xy] = float4(blend.xxx, original.a);
		return;
	}

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
