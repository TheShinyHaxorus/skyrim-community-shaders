/**
 * Motion Blur - Horizontal Reduction Pass (Pass 1a of 3)
 *
 * Performs horizontal reduction from [width x height] to [grid x height]
 * Each thread processes a horizontal strip of input pixels
 * First stage of the separable approach for maximum velocity calculation
 */

#include "PostProcessing/common.hlsli"

// Textures
Texture2D<float4> TexVelocity : register(t0);        // Full resolution motion vectors
RWTexture2D<float4> RWTexHorizontal : register(u0);  // [20 x height] output

// Constants
cbuffer MotionBlurCB : register(b0)
{
	float4 g_VelocityParams;    // x: shutter fraction, yz: motion-vector source scale, w: max blur radius in px
	float4 g_TargetResolution;  // xy: rendered area of the color target, in pixels
}

// Fixed parameters
static const uint GRID_SIZE = 20;  // Fixed grid size

uint2 GetVelocitySamplePosition(uint2 targetPixel, uint2 velocityDimensions)
{
	float2 velocityScale = g_VelocityParams.yz;
	float2 sourceDimensionsF = max(float2(1.0f, 1.0f), g_TargetResolution.xy * velocityScale);
	uint2 sourceDimensions = min(velocityDimensions, uint2(sourceDimensionsF + 0.5f));
	uint2 sourcePixel = uint2((float2(targetPixel) + 0.5f) * velocityScale);
	return min(sourcePixel, sourceDimensions - 1);
}

// Process horizontal strips
[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Get dimensions and check bounds
	uint2 velocityDimensions;
	TexVelocity.GetDimensions(velocityDimensions.x, velocityDimensions.y);

	uint2 targetDimensions = uint2(max(g_TargetResolution.xy, float2(1.0f, 1.0f)) + 0.5f);

	uint2 horizontalDimensions;
	RWTexHorizontal.GetDimensions(horizontalDimensions.x, horizontalDimensions.y);

	if (DTid.x >= horizontalDimensions.x || DTid.y >= horizontalDimensions.y)
		return;

	if (DTid.y >= targetDimensions.y) {
		RWTexHorizontal[DTid.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}

	// Calculate horizontal range to process
	uint xTileSize = (targetDimensions.x + GRID_SIZE - 1) / GRID_SIZE;
	uint xStart = DTid.x * xTileSize;
	uint xEnd = min(xStart + xTileSize, targetDimensions.x);

	// Track maximum velocity
	float maxVelocityMagnitude = 0.0f;
	float2 maxVelocity = float2(0.0f, 0.0f);

	// Process horizontal strip
	for (uint x = xStart; x < xEnd; x++) {
		uint2 velocityPixel = GetVelocitySamplePosition(uint2(x, DTid.y), velocityDimensions);

		float2 velocity = TexVelocity[velocityPixel].xy * g_TargetResolution.xy;
		velocity *= g_VelocityParams.x;

		float velocityMagnitude = length(velocity);
		if (velocityMagnitude > maxVelocityMagnitude) {
			maxVelocityMagnitude = velocityMagnitude;
			maxVelocity = velocity;
		}
	}

	RWTexHorizontal[DTid.xy] = float4(maxVelocity, maxVelocityMagnitude, 1.0f);
}
