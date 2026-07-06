/////////////////////////////////////////////////////////////////////////////////////////////
// Copyright 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/////////////////////////////////////////////////////////////////////////////////////////////

#ifndef H_AOIT
#define H_AOIT
#include "OIT/OITCommon.hlsli"

float UnpackUnnormAlpha(uint packedInput)
{
	return (float) (packedInput >> 24UL) / 255;
}

uint PackUnnormAlpha(float unpackedInput)
{
	return (uint) (unpackedInput * 255 + 0.5);
}

float3 UnpackRGB(uint packedInput)
{
	float3 unpackedOutput;
	uint3 p = uint3((packedInput & 0xFFUL),
		(packedInput >> 8UL) & 0xFFUL,
		(packedInput >> 16UL) & 0xFFUL);

	unpackedOutput = ((float3) p) / 255;
	return unpackedOutput;
}

uint PackRGB(float3 unpackedInput)
{
	uint3 u = (uint3) (saturate(unpackedInput) * 255 + 0.5);
	uint packedOutput = (u.z << 16UL) | (u.y << 8UL) | u.x;
	return packedOutput;
}

uint PackRGBA(float4 unpackedInput)
{
	uint4 u = (uint4) (saturate(unpackedInput) * 255 + 0.5);
	uint packedOutput = (u.w << 24UL) | (u.z << 16UL) | (u.y << 8UL) | u.x;
	return packedOutput;
}

float4 UnpackRGBA(uint packedInput)
{
	float4 unpackedOutput;
	uint4 p = uint4((packedInput & 0xFFUL),
		(packedInput >> 8UL) & 0xFFUL,
		(packedInput >> 16UL) & 0xFFUL,
		(packedInput >> 24UL));

	unpackedOutput = ((float4) p) / 255;
	return unpackedOutput;
}

//////////////////////////////////////////////
// Structs
//////////////////////////////////////////////

struct AOITCtrlSurface
{
	bool clear;
	bool opaque;
	float depth;
};

struct AOITSPData
{
	float4 depth[OIT_RT_COUNT];
	uint4 color[OIT_RT_COUNT];
};

struct AOITSPDepthData
{
	float4 depth[OIT_RT_COUNT];
};

struct AOITSPColorData
{
	uint4 color[OIT_RT_COUNT];
};

struct ATSPNode
{
	float depth;
	float trans;
	uint color;
};

//////////////////////////////////////////////
// Resources
//////////////////////////////////////////////
// OIT Collection phase UAV
#define OIT_USE_ROV 1
#if defined(OIT_USE_ROV)
RasterizerOrderedTexture2D<uint>    gAOITSPClearMaskUAV : register(u3);
#else
RWTexture2D<uint>                   gAOITSPClearMaskUAV : register(u3);
#endif
RWStructuredBuffer<AOITSPColorData> gAOITSPColorDataUAV : register(u4);
RWStructuredBuffer<AOITSPDepthData> gAOITSPDepthDataUAV : register(u5);

// OIT Resolve phase SRV
Texture2D<uint>                     gAOITSPClearMaskSRV : register(t1);
StructuredBuffer<AOITSPColorData>   gAOITSPColorDataSRV : register(t2);
StructuredBuffer<AOITSPDepthData>   gAOITSPDepthDataSRV : register(t3);

//////////////////////////////////////////////
// Main AOIT fragment insertion code
//////////////////////////////////////////////

//////////////////////////////////////////////
// Main AOIT fragment insertion code
//////////////////////////////////////////////

void AOITSPInsertFragment(in float fragmentDepth,
						  in float fragmentTrans,
						  in float3 fragmentColor,
						  inout ATSPNode nodeArray[OIT_NODE_COUNT])
{
	int i, j;

	float depth[OIT_NODE_COUNT + 1];
	float trans[OIT_NODE_COUNT + 1];
	uint color[OIT_NODE_COUNT + 1];

	///////////////////////////////////////////////////
	// Unpack AOIT data
	///////////////////////////////////////////////////                   
	[unroll]
	for (i = 0; i < OIT_NODE_COUNT; ++i)
	{
		depth[i] = nodeArray[i].depth;
		trans[i] = nodeArray[i].trans;
		color[i] = nodeArray[i].color;
	}
	
	// Find insertion index 
	int index = 0;
	float prevTrans = 1;
	[unroll]
	for (i = 0; i < OIT_NODE_COUNT; ++i)
	{
		if (depth[i] < fragmentDepth)
		{
			index++;
			prevTrans = trans[i];
		}
	}

	// Make room for the new fragment. Also composite new fragment with the current curve 
	// (except for the node that represents the new fragment)
	[unroll]
	for (i = OIT_NODE_COUNT - 1; i >= 0; --i)
	{
		[flatten]
		if (i >= index)
		{
			depth[i + 1] = depth[i];
			trans[i + 1] = trans[i] * fragmentTrans;
			color[i + 1] = color[i];
		}
	}
	
	// Insert new fragment
	const float newFragTrans = fragmentTrans * prevTrans;
	const uint newFragColor = PackRGB(fragmentColor);
	depth[index] = fragmentDepth;
	trans[index] = newFragTrans;
	color[index] = newFragColor;
	//[unroll]for (i = 0; i <= OIT_NODE_COUNT; ++i) {
	//	[flatten]if (index == i) {
	//		depth[i] = fragmentDepth;
	//		trans[i] = newFragTrans;
	//		color[i] = newFragColor;
	//	}
	//} 

	float EMPTY_NODE = asfloat(asuint(OIT_EMPTY_NODE_DEPTH) & OIT_TRANS_MASK);

	[flatten]
	if (asfloat(asuint(float(depth[OIT_NODE_COUNT])) & OIT_TRANS_MASK) != EMPTY_NODE)
	{
		float3 toBeRemovedCol = UnpackRGB(color[OIT_NODE_COUNT]);
		float3 toBeAccumulCol = UnpackRGB(color[OIT_NODE_COUNT - 1]);
		color[OIT_NODE_COUNT - 1] = PackRGB(toBeAccumulCol + toBeRemovedCol * trans[OIT_NODE_COUNT - 1] * rcp(trans[OIT_NODE_COUNT - 2]));
		trans[OIT_NODE_COUNT - 1] = trans[OIT_NODE_COUNT];
	}

	// Pack AOIT data
	[unroll]
	for (i = 0; i < OIT_NODE_COUNT; ++i)
	{
		nodeArray[i].depth = depth[i];
		nodeArray[i].trans = trans[i];
		nodeArray[i].color = color[i];
	}
}

/////////////////////////////////////////////////
// Address generation functions for the AOIT data
/////////////////////////////////////////////////

uint AOITAddrGen(uint2 addr2D, uint surfaceWidth)
{
#ifdef OIT_TILED_ADDRESSING
	surfaceWidth = surfaceWidth >> 1U;
	uint2 tileAddr2D = addr2D >> 1U;
	uint tileAddr1D = (tileAddr2D[0] + surfaceWidth * tileAddr2D[1]) << 2U;
	uint2 pixelAddr2D = addr2D & 0x1U;
	uint pixelAddr1D = (pixelAddr2D[1] << 1U) + pixelAddr2D[0];
	
	return tileAddr1D | pixelAddr1D;
#else
	return addr2D[0] + surfaceWidth * addr2D[1];	
#endif
}

uint AOITAddrGenUAV(uint2 addr2D)
{
	uint2 dim;
	gAOITSPClearMaskUAV.GetDimensions(dim[0], dim[1]);
	return AOITAddrGen(addr2D, dim[0]);
}

uint AOITAddrGenSRV(uint2 addr2D)
{
	uint2 dim;
	gAOITSPClearMaskSRV.GetDimensions(dim[0], dim[1]);
	return AOITAddrGen(addr2D, dim[0]);
}

void AOITSPClearData(inout AOITSPData data, float depth, float4 color)
{
	uint packedColor = PackRGBA(float4(0, 0, 0, 1.0f - color.w));

	[unroll]
	for (uint i = 0; i < OIT_RT_COUNT; i++)
	{
		data.depth[i] = OIT_EMPTY_NODE_DEPTH;
		data.color[i] = packedColor;
	}
	data.depth[0][0] = depth;
	data.color[0][0] = PackRGBA(float4(color.xyz, 1.0f - color.w));
}
/////////////////////////////////////////////////
// Load/store functions for the AOIT data
/////////////////////////////////////////////////

void AOITSPLoadDataSRV(in uint2 pixelAddr, out ATSPNode nodeArray[OIT_NODE_COUNT])
{
	AOITSPData data;
	uint addr = AOITAddrGenSRV(pixelAddr);
	data.color = gAOITSPColorDataSRV[addr];
	data.depth = gAOITSPDepthDataSRV[addr];

	[unroll]
	for (uint i = 0; i < OIT_RT_COUNT; i++)
	{
		[unroll]
		for (uint j = 0; j < 4; j++)
		{
			ATSPNode node =
			{
				data.depth[i][j], UnpackUnnormAlpha(data.color[i][j]),
				data.color[i][j] & 0xFFFFFFUL
			};
			nodeArray[4 * i + j] = node;
		}
	}
}



void AOITSPLoadDataUAV(in uint2 pixelAddr, out ATSPNode nodeArray[OIT_NODE_COUNT])
{
	AOITSPData data;
	uint addr = AOITAddrGenUAV(pixelAddr);
	data.color = gAOITSPColorDataUAV[addr];

	data.depth = gAOITSPDepthDataUAV[addr];
	[unroll]
	for (uint i = 0; i < OIT_RT_COUNT; i++)
	{
		[unroll]
		for (uint j = 0; j < 4; j++)
		{
			ATSPNode node =
			{
				data.depth[i][j],
				UnpackUnnormAlpha(data.color[i][j]),
				data.color[i][j] & 0xFFFFFFUL
			};
			nodeArray[4 * i + j] = node;
		}
	}
}

void AOITSPStoreDataUAV(in uint2 pixelAddr, ATSPNode nodeArray[OIT_NODE_COUNT])
{
	AOITSPData data;
	uint addr = AOITAddrGenUAV(pixelAddr);

	[unroll]
	for (uint i = 0; i < OIT_RT_COUNT; i++)
	{
		[unroll]
		for (uint j = 0; j < 4; j++)
		{
			data.depth[i][j] = nodeArray[4 * i + j].depth;
			data.color[i][j] = (nodeArray[4 * i + j].color & 0xFFFFFFUL) |
				(PackUnnormAlpha(nodeArray[4 * i + j].trans) << 24UL);
		}
	}
	gAOITSPDepthDataUAV[addr] = data.depth;
	gAOITSPColorDataUAV[addr] = data.color;
}


/////////////////////////////////////////////////////////////
// Control Surface functions for the AOIT data
// We use this surface to remove the overhead incurred in 
// clearing large AOIT buffers by storing for each
// pixel on the screen a to-be-cleared flag.
// We use the same structure to store some additional
// per-pixel information such as the depth of the most
// distant transparent fragment and its total transmittance,
// which in turn can be used to perform early-z culling over
// pixels covered by transparent fragments
/////////////////////////////////////////////////////////////

void AOITLoadControlSurface(in uint data, inout AOITCtrlSurface surface)
{
	surface.clear = data & 0x1 ? false : true; // 0 == clear, 1 == not clear
	surface.opaque = data & 0x2 ? true : false;
	surface.depth = asfloat((data & 0xFFFFFFFCUL) | 0x3UL);
}

void AOITLoadControlSurfaceUAV(in uint2 pixelAddr, inout AOITCtrlSurface surface)
{
	uint data = gAOITSPClearMaskUAV[pixelAddr];
	AOITLoadControlSurface(data, surface);
}

void AOITLoadControlSurfaceSRV(in uint2 pixelAddr, inout AOITCtrlSurface surface)
{
	uint data = gAOITSPClearMaskSRV[pixelAddr];
	AOITLoadControlSurface(data, surface);
}

void WriteNewPixelToAOIT(in uint2 pixelAddr, in float surfaceDepth, in float4 surfaceColor)
{
	// From now on serialize all UAV accesses (with respect to other fragments shaded in flight which map to the same pixel)
	ATSPNode nodeArray[OIT_NODE_COUNT];

	// Load AOIT control surface
	AOITCtrlSurface ctrlSurface;
	AOITLoadControlSurfaceUAV(pixelAddr, ctrlSurface);

	// If we are modifying this pixel for the first time we need to clear the AOIT data
	if (ctrlSurface.clear)
	{
		// Clear AOIT data and initialize it with first transparent layer
		AOITSPData data;
		AOITSPClearData(data, surfaceDepth, surfaceColor);

		// Store AOIT data
		uint addr = AOITAddrGenUAV(pixelAddr);
		gAOITSPDepthDataUAV[addr] = data.depth;
		gAOITSPColorDataUAV[addr] = data.color;
		gAOITSPClearMaskUAV[pixelAddr] = 1;
	}
	else
	{
		// Load AOIT data
		AOITSPLoadDataUAV(pixelAddr, nodeArray);

		// Update AOIT data
		AOITSPInsertFragment(surfaceDepth,
							 1.0f - surfaceColor.w, // transmittance = 1 - alpha
							 surfaceColor.xyz,
							 nodeArray);
		// Store AOIT data
		AOITSPStoreDataUAV(pixelAddr, nodeArray);
	}
}

bool OIT_CaptureImpl(in int2 screenAddress, in float4 color, in float depth, in uint flags)
{
	WriteNewPixelToAOIT(screenAddress, depth, color);
	return true;
}

#endif // H_AOIT

