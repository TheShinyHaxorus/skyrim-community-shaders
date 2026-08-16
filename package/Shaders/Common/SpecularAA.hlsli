#ifndef __SPECULAR_AA_HLSLI__
#define __SPECULAR_AA_HLSLI__

#include "Common/SharedData.hlsli"

#if defined(PSHADER)

// Geometric specular antialiasing [Kaplanyan et al. 2016; Tokuyoshi and Kaplanyan 2021].
namespace SpecularAA
{
	float NormalVariance(float3 normal)
	{
		float3 dndx = ddx(normal);
		float3 dndy = ddy(normal);
		return dot(dndx, dndx) + dot(dndy, dndy);
	}

	// Roughness is perceptual, so the kernel is added in alpha^2 space where alpha = r * r.
	float FilterRoughness(float perceptualRoughness, float normalVariance)
	{
		float variance = SharedData::specularAASettings.Variance * normalVariance;
		float kernelRoughness = min(2.0 * variance, SharedData::specularAASettings.Threshold);
		float alpha = perceptualRoughness * perceptualRoughness;
		float filtered = sqrt(sqrt(saturate(alpha * alpha + kernelRoughness)));
		return SharedData::specularAASettings.Enable ? filtered : perceptualRoughness;
	}

	float FilterRoughness(float perceptualRoughness, float3 normal)
	{
		return FilterRoughness(perceptualRoughness, NormalVariance(normal));
	}
}

#endif  // PSHADER

#endif  // __SPECULAR_AA_HLSLI__
