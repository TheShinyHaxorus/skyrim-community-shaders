struct VS_OUTPUT
{
	float4 Position: SV_POSITION;
	float2 TexCoord: TEXCOORD;
};

#if defined(VSHADER)

VS_OUTPUT main(uint vertexId: SV_VertexID)
{
	VS_OUTPUT vsout;

	vsout.Position.x = (float)(vertexId / 2) * 4.0 - 1.0;
	vsout.Position.y = (float)(vertexId % 2) * 4.0 - 1.0;
	vsout.Position.z = 0.0;
	vsout.Position.w = 1.0;

	vsout.TexCoord.x = (float)(vertexId / 2) * 2.0;
	vsout.TexCoord.y = 1.0 - (float)(vertexId % 2) * 2.0;

	return vsout;
}

#endif

#if defined(PSHADER)

cbuffer UnderwaterCB: register(b0)
{
	float4 Params0;       // x = BlurAmount, y = BlurRange, z = DistortionAmount, w = DistortionScale
	float4 Params1;       // x = DistortionSpeed, y = Strength, z = NoiseScale, w = Timer
	float4 ScreenParams;  // x = 1 / Width, y = 1 / Height, z = VerticalAmount, w = unused
	float4 DynRes;        // xy = DynamicResolutionParams1.xy, zw = unused
};

SamplerState LinearClampSampler: register(s0);
SamplerState PointClampSampler: register(s1);

Texture2D<float4> TexColor: register(t0);
#	if defined(PASS_DRAW)
Texture2D<float4> TexOriginal: register(t1);
#	else
Texture2D<float> TexDepth: register(t1);
#	endif

float2 AdjustDepthUV(float2 uv)
{
	return uv * DynRes.xy;
}

float2 DistortImage(float2 uv)
{
	float iTime = Params1.w * 1677.7216 * Params1.x;

	float X = uv.x * Params0.w + iTime;
	float Y = uv.y * Params0.w + iTime;
	float amount = 0.0025 * Params0.z;
	uv.y += cos(X + Y) * amount * cos(Y) * ScreenParams.z;
	uv.x += sin(X - Y) * amount * sin(Y);

	return saturate(uv);
}

float HashNoise(float2 p)
{
	return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float ValueNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	float2 u = f * f * (3.0 - 2.0 * f);
	float a = HashNoise(i);
	float b = HashNoise(i + float2(1.0, 0.0));
	float c = HashNoise(i + float2(0.0, 1.0));
	float d = HashNoise(i + float2(1.0, 1.0));
	return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

#	if defined(PASS_BLUR)

float4 main(VS_OUTPUT input): SV_Target
{
	const float2 offsets[4] = {
		float2(-1.0, -1.0),
		float2(-1.0, 1.0),
		float2(1.0, -1.0),
		float2(1.0, 1.0),
	};

	float2 uv = input.TexCoord;
	float2 pixeloffset = Params0.y * ScreenParams.xy;

	float3 centercolor = TexColor.SampleLevel(LinearClampSampler, uv, 0).xyz;
	float depth = TexDepth.SampleLevel(PointClampSampler, AdjustDepthUV(uv), 0).x;

	float centerweight = saturate(depth * depth);
	centerweight = 1.000001 - centerweight * centerweight;

	float3 color = centercolor * centerweight;
	float weight = centerweight;

	[unroll] for (int i = 0; i < 4; i++)
	{
		float2 coord = offsets[i] * pixeloffset + uv;
		float tempdepth = TexDepth.SampleLevel(PointClampSampler, AdjustDepthUV(coord), 0).x;

		float tempweight = saturate(tempdepth * tempdepth);
		tempweight = tempweight * tempweight;

		color += TexColor.SampleLevel(LinearClampSampler, DistortImage(coord), 0).xyz * tempweight;
		weight += tempweight;
	}
	color /= weight;

	float3 res = lerp(centercolor, color, Params0.x);
	return float4(res, 1.0);
}

#	elif defined(PASS_DRAW)

float4 main(VS_OUTPUT input): SV_Target
{
	float2 uv = input.TexCoord;

	float blurredmask = Params1.y;
	float mask = blurredmask;

	float noiseY = frac(Params1.w * 1677.7216 * 2.0);
	float anim = ValueNoise(float2(uv.x * Params1.z, noiseY * Params1.z));
	anim = anim * 2.0 - 1.0;

	float2 coord = uv;
	coord.y -= (1.0 - blurredmask) * anim * 0.025 * ScreenParams.z;

	float3 originalcolor = TexOriginal.SampleLevel(LinearClampSampler, uv, 0).xyz;
	float3 color = TexColor.SampleLevel(LinearClampSampler, coord, 0).xyz;

	blurredmask = 1.0 - abs(blurredmask * 2.0 - 1.0);

	float3 res = lerp(originalcolor, color, saturate(blurredmask));
	res = lerp(res, color, saturate(mask));

	return float4(res, 1.0);
}

#	endif

#endif
