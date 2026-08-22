/**
 * Motion Blur - Vertical Reduction Pass (Pass 1b of 3)
 *
 * Performs vertical reduction from [grid × height] to [grid × grid]
 * Each column of threads processes a vertical strip of the input texture
 * Preserves the maximum velocity vectors for each grid cell
 */

#include "PostProcessing/common.hlsli"

// Textures
Texture2D<float4> TexHorizontal : register(t0);    // [20 x height] input
RWTexture2D<float4> RWTexVertical : register(u0);  // [20 x 20] output

// Constants
cbuffer MotionBlurCB : register(b0)
{
	float4 g_VelocityParams;    // x: shutter fraction, yz: motion-vector source scale, w: max blur radius in px
	float4 g_TargetResolution;  // xy: rendered area of the color target, in pixels
}

// Fixed parameters
static const uint GRID_SIZE = 20;  // Fixed grid size

// Vertical reduction pass
[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID) {
	// Get dimensions and check bounds
	uint2 horizontalDimensions;
	TexHorizontal.GetDimensions(horizontalDimensions.x, horizontalDimensions.y);

	if (DTid.x >= GRID_SIZE || DTid.y >= GRID_SIZE)
		return;

	uint targetHeight = min(horizontalDimensions.y, uint(max(g_TargetResolution.y, 1.0f) + 0.5f));

	// Calculate vertical range to process
	uint yTileSize = (targetHeight + GRID_SIZE - 1) / GRID_SIZE;
	uint yStart = DTid.y * yTileSize;
	uint yEnd = min(yStart + yTileSize, targetHeight);

	// Track maximum velocity
	float maxVelocityMagnitude = 0.0f;
	float2 maxVelocity = float2(0.0f, 0.0f);

	// Process vertical strip
	for (uint y = yStart; y < yEnd; y++) {
		float4 sampleVelocity = TexHorizontal[uint2(DTid.x, y)];

		if (sampleVelocity.z > maxVelocityMagnitude) {
			maxVelocityMagnitude = sampleVelocity.z;
			maxVelocity = sampleVelocity.xy;
		}
	}

	RWTexVertical[DTid.xy] = float4(maxVelocity, maxVelocityMagnitude, 1.0f);
}
