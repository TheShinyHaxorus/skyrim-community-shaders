/**
 * Motion Blur - Blur Pass (Pass 3 of 3)
 *
 * Final pass that applies motion blur using neighborhood velocities
 * Uses depth-aware sampling to prevent foreground/background bleeding
 */

#include "PostProcessing/common.hlsli"

// Textures and buffers
Texture2D<float4> TexColor : register(t0);        // Color buffer
Texture2D<float4> TexVelocity : register(t1);     // Motion vectors
Texture2D<float4> TexNeighborMax : register(t2);  // Neighborhood velocities (20×20 grid)
Texture2D<float> TexDepth : register(t3);         // Depth buffer
RWTexture2D<float4> RWTexOut : register(u0);      // Output

// Samplers
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

// Constants
cbuffer MotionBlurCB : register(b0)
{
	float4 g_VelocityParams;    // x: shutter fraction, yz: motion-vector source scale, w: max blur radius in px
	float4 g_TargetResolution;  // xy: rendered area of the color target, in pixels
	float4 g_DepthParams;       // far, near, far - near, far * near
	int g_SampleCount;          // Number of samples
}

// Fixed parameters
static const uint GRID_SIZE = 20;  // Fixed grid size
#define MB_SOFTZ_UNITS 2.0f

float LinearizeDepth(float depth)
{
	return g_DepthParams.w / (-depth * g_DepthParams.z + g_DepthParams.x);
}

// Depth comparison - returns (foreground, background) weights
float2 DepthCmp(float centerDepth, float sampleDepth, float depthScale)
{
	return saturate(0.5f + float2(depthScale, -depthScale) * (sampleDepth - centerDepth));
}

// Spread comparison
float2 SpreadCmp(float offsetLen, float2 spreadLen, float pixelToSampleUnitsScale)
{
	return saturate(pixelToSampleUnitsScale * spreadLen - max(offsetLen - 1.0f, 0.0f));
}

// Calculate sample weight
float SampleWeight(
	float centerDepth, float sampleDepth, float offsetLen, float centerSpreadLen,
	float sampleSpreadLen, float pixelToSampleUnitsScale, float depthScale)
{
	float2 depthCmp = DepthCmp(centerDepth, sampleDepth, depthScale);
	float2 spreadCmp = SpreadCmp(offsetLen, float2(centerSpreadLen, sampleSpreadLen), pixelToSampleUnitsScale);
	return dot(depthCmp, spreadCmp);
}

// Generate dithered offset
float GetDitheredOffset(uint2 position, float sampleIndex)
{
	float scale = 0.25f;
	uint2 positionMod = position & 1;  // 2x2 checkerboard
	return (-scale + 2.0f * scale * positionMod.x) * (-1.0f + 2.0f * positionMod.y);
}

float2 GetVelocityTexCoord(float2 targetTexCoord)
{
	uint2 targetDimensions;
	TexColor.GetDimensions(targetDimensions.x, targetDimensions.y);

	uint2 velocityDimensions;
	TexVelocity.GetDimensions(velocityDimensions.x, velocityDimensions.y);

	float2 targetToVelocityScale = g_VelocityParams.yz;
	float2 sourceDimensions = max(float2(1.0f, 1.0f), float2(targetDimensions) * targetToVelocityScale);
	sourceDimensions = min(sourceDimensions, float2(velocityDimensions));

	float2 uvScale = sourceDimensions / float2(velocityDimensions);
	float2 minUV = 0.5f / float2(velocityDimensions);
	float2 maxUV = (sourceDimensions - 0.5f) / float2(velocityDimensions);

	return clamp(targetTexCoord * uvScale, minUV, maxUV);
}

float2 SampleVelocity(float2 targetTexCoord)
{
	float2 velocity = TexVelocity.SampleLevel(PointSampler, GetVelocityTexCoord(targetTexCoord), 0).xy;
	return velocity * g_TargetResolution.xy * g_VelocityParams.x;
}

// Main function
[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Get dimensions and check bounds
	uint2 dimensions;
	TexColor.GetDimensions(dimensions.x, dimensions.y);

	if (DTid.x >= dimensions.x || DTid.y >= dimensions.y)
		return;

	uint2 pixelPos = DTid.xy;

	// Sample center pixel data
	float2 texCoord = (pixelPos + 0.5f) / float2(dimensions);
	float4 centerColor = TexColor.SampleLevel(LinearSampler, texCoord, 0);
	float centerDepth = LinearizeDepth(TexDepth.SampleLevel(PointSampler, texCoord, 0));
	float2 centerVelocity = SampleVelocity(texCoord);

	// Calculate tile coordinates
	uint2 targetDimensions = uint2(max(g_TargetResolution.xy, float2(1.0f, 1.0f)) + 0.5f);

	uint2 gridCoord;
	gridCoord.x = (pixelPos.x * GRID_SIZE) / targetDimensions.x;
	gridCoord.y = (pixelPos.y * GRID_SIZE) / targetDimensions.y;

	uint2 neighborMaxDimensions;
	TexNeighborMax.GetDimensions(neighborMaxDimensions.x, neighborMaxDimensions.y);

	gridCoord = min(gridCoord, neighborMaxDimensions - 1);

	// Get max velocity from grid
	float4 neighborMaxSample = TexNeighborMax[gridCoord];
	float2 neighborMaxVelocity = neighborMaxSample.xy;
	float neighborMaxLength = neighborMaxSample.z;

	// Determine blur direction and length
	float2 blurDir = neighborMaxLength > 0.001f ? neighborMaxVelocity / neighborMaxLength : float2(0.0f, 0.0f);
	float blurLength = min(neighborMaxLength, g_VelocityParams.w);

	// Skip if no motion
	if (blurLength < 0.5f) {
		RWTexOut[pixelPos] = centerColor;
		return;
	}

	float centerVelocityLen = length(centerVelocity);

	// Initialize for sampling
	uint sampleCount = uint(max(g_SampleCount, 2));
	uint samplePairs = sampleCount / 2u;

	float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
	float pixelToSampleUnitsScale = float(sampleCount) / blurLength;
	float depthScale = 1.0f / MB_SOFTZ_UNITS;

	// Sample in pairs (mirrored)
	for (uint i = 0; i < samplePairs; i++) {
		// Calculate offset
		float offset = (float(i) + 0.5f) / float(samplePairs) * blurLength;
		offset += GetDitheredOffset(pixelPos, i);

		// Sample pairs in opposite directions
		float2 pixelOffsetFwd = blurDir * offset;
		float2 pixelOffsetBck = -pixelOffsetFwd;

		float2 sampleTexCoordsFwd = (pixelPos + pixelOffsetFwd + 0.5f) / float2(dimensions);
		float2 sampleTexCoordsBck = (pixelPos + pixelOffsetBck + 0.5f) / float2(dimensions);

		// Sample depth and velocity
		float sampleDepthFwd = LinearizeDepth(TexDepth.SampleLevel(PointSampler, sampleTexCoordsFwd, 0));
		float sampleDepthBck = LinearizeDepth(TexDepth.SampleLevel(PointSampler, sampleTexCoordsBck, 0));

		float sampleVelocityLenFwd = length(SampleVelocity(sampleTexCoordsFwd));
		float sampleVelocityLenBck = length(SampleVelocity(sampleTexCoordsBck));

		float offsetLen = offset;

		// Calculate sample weights
		float weightFwd = SampleWeight(
			centerDepth, sampleDepthFwd, offsetLen, centerVelocityLen, sampleVelocityLenFwd,
			pixelToSampleUnitsScale, depthScale);

		float weightBck = SampleWeight(
			centerDepth, sampleDepthBck, offsetLen, centerVelocityLen, sampleVelocityLenBck,
			pixelToSampleUnitsScale, depthScale);

		// Sample colors and accumulate
		float4 sampleColorFwd = TexColor.SampleLevel(LinearSampler, sampleTexCoordsFwd, 0);
		float4 sampleColorBck = TexColor.SampleLevel(LinearSampler, sampleTexCoordsBck, 0);

		sum += weightFwd * float4(sampleColorFwd.rgb, 1.0f);
		sum += weightBck * float4(sampleColorBck.rgb, 1.0f);
	}

	// Normalize
	sum.rgb *= 1.0f / float(sampleCount);
	sum.w *= 1.0f / float(sampleCount);

	// Final blend with background
	float4 outputColor = float4(
		sum.rgb + (1.0f - sum.w) * centerColor.rgb,
		1.0f);

	// Write output
	RWTexOut[pixelPos] = outputColor;
}
