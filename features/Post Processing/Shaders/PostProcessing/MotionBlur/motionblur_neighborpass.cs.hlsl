/**
 * Motion Blur - Neighbor Max Pass (Pass 2 of 3)
 *
 * Examines 3x3 grid neighborhoods to find maximum velocities
 * Handles diagonal neighbors by checking if velocity points toward current cell
 * Outputs final neighborhood-aware velocity data for blur pass
 */

#include "PostProcessing/common.hlsli"

// Textures
Texture2D<float4> TexVertical : register(t0);         // [20×20] input from vertical pass
RWTexture2D<float4> RWTexNeighborMax : register(u0);  // [20×20] output

// Constants
cbuffer MotionBlurCB : register(b0)
{
	float4 g_VelocityParams;    // x: shutter fraction, yz: motion-vector source scale, w: max blur radius in px
	float4 g_TargetResolution;  // xy: rendered area of the color target, in pixels
}

// Fixed parameters
static const uint GRID_SIZE = 20;  // Fixed grid size

// Process one cell in the grid
[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Check bounds
	if (DTid.x >= GRID_SIZE || DTid.y >= GRID_SIZE)
		return;

	uint2 cellIndex = DTid.xy;

	// Track maximum velocity
	float2 maxVelocity = float2(0.0f, 0.0f);
	float maxVelocityMagnitude = 0.0f;

	// Check 3x3 neighborhood
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			// Get neighbor coordinates
			int2 neighborCellIndex = int2(cellIndex) + int2(x, y);

			// Skip out-of-bounds neighbors
			if (neighborCellIndex.x < 0 || neighborCellIndex.y < 0 ||
				neighborCellIndex.x >= int(GRID_SIZE) ||
				neighborCellIndex.y >= int(GRID_SIZE))
				continue;

			// Get neighbor velocity
			float4 neighborSample = TexVertical[neighborCellIndex];
			float2 neighborVelocity = neighborSample.xy;
			float neighborVelocityMagnitude = neighborSample.z;

			if (neighborVelocityMagnitude <= 0.0f)
				continue;

			// For diagonal neighbors, only include if velocity points toward current cell
			bool isDiagonal = (x != 0 && y != 0);
			if (isDiagonal) {
				// Direction from neighbor to current cell
				float2 dirToCurrentCell = normalize(float2(-x, -y));

				// Neighbor velocity direction
				float2 neighborDir = neighborVelocity / neighborVelocityMagnitude;

				// Skip if velocity doesn't point toward current cell
				if (dot(neighborDir, dirToCurrentCell) <= 0.0f)
					continue;
			}

			// Keep largest velocity
			if (neighborVelocityMagnitude > maxVelocityMagnitude) {
				maxVelocityMagnitude = neighborVelocityMagnitude;
				maxVelocity = neighborVelocity;
			}
		}
	}

	RWTexNeighborMax[DTid.xy] = float4(maxVelocity, maxVelocityMagnitude, 1.0f);
}
