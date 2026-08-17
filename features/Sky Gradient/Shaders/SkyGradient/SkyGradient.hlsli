#ifndef __SKY_GRADIENT_HLSLI__
#define __SKY_GRADIENT_HLSLI__

#include "Common/Color.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#if defined(PSHADER)

namespace SkyGradient
{
	float3 NormalizeWeights(float3 weights)
	{
		weights = max(weights, 0.0);
		float total = weights.x + weights.y + weights.z;
		return total > 1e-5 ? weights / total : float3(0.0, 0.0, 1.0);
	}

	float3 SmoothWeights(float3 weights, float amount)
	{
		float3 smoothed = weights * weights * (3.0 - 2.0 * weights);
		return NormalizeWeights(lerp(weights, smoothed, amount));
	}

	float3 BlendStops(float3 a_vanilla, float3 a_weights, float3 a_horizon, float3 a_middle, float3 a_zenith)
	{
		float3 weights = NormalizeWeights(a_weights);
		weights = SmoothWeights(weights, SharedData::skyGradientSettings.Smoothing);

		float3 labHorizon = Color::Correct::BT709ToOKLab(a_horizon);
		float3 labMiddle = Color::Correct::BT709ToOKLab(a_middle);
		float3 labZenith = Color::Correct::BT709ToOKLab(a_zenith);

		float3 lab = labHorizon * weights.x + labMiddle * weights.y + labZenith * weights.z;

		float chromaTarget = length(labHorizon.yz) * weights.x + length(labMiddle.yz) * weights.y + length(labZenith.yz) * weights.z;
		float chromaMixed = length(lab.yz);
		[branch] if (chromaMixed > 1e-5)
		{
			float restored = lerp(chromaMixed, chromaTarget, SharedData::skyGradientSettings.ChromaPreservation);
			lab.yz *= restored / chromaMixed;
		}

		lab.x = Color::Correct::BT709ToOKLab(a_vanilla).x;

		return max(Color::Correct::OkLabToBT709(lab), 0.0);
	}

	float3 Dither(float3 color, float2 screenPos)
	{
		float amplitude = SharedData::skyGradientSettings.DitherStrength * (1.5 / 255.0);
		float3 scale = amplitude + abs(color) * (SharedData::skyGradientSettings.DitherStrength * 0.03);
		return Random::TriDither(screenPos, SharedData::FrameCount) * scale;
	}
}

#endif  // PSHADER

#endif  // __SKY_GRADIENT_HLSLI__
