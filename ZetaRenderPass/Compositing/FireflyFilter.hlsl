#include "Compositing_Common.h"
#include "../Common/BRDF.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/RT.hlsli"
#include "../Common/VolumetricLighting.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFireflyFilter> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

float GeometryTest(float sampleLinearDepth, float2 sampleUV, float3 centerNormal, float3 centerPos, float centerLinearDepth)
{
	float3 samplePos = Math::Transform::WorldPosFromUV(sampleUV,
		sampleLinearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	float planeDist = dot(centerNormal, samplePos - centerPos);
	float weight = abs(planeDist) <= 0.01 * centerLinearDepth;
	
	return weight;
}

// Ref: P. Kozlowski and T. Cheblokov, "ReLAX: A Denoiser Tailored to Work with the ReSTIR Algorithm," GTC, 2021.
float3 FilterFirefly(Texture2D<float4> g_input, float3 currColor, int2 DTid, int2 GTid, float linearDepth, float3 normal, float3 posW)
{
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];

	float minLum = FLT_MAX;
	float maxLum = 0.0;
	float3 minColor = currColor;
	float3 maxColor = currColor;
	float currLum = Math::Color::LuminanceFromLinearRGB(currColor);
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 rcpRenderDim = 1.0f / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	[unroll]
	for (int i = -1; i <= 1; i++)
	{
		[unroll]
		for (int j = -1; j <= 1; j++)
		{
			if (i == 0 && j == 0)
				continue;
			
			int2 addr = int2(DTid.x + j, DTid.y + i);
			if (any(addr) < 0 || any(addr >= renderDim))
				continue;
			
			const float neighborLinearDepth = Math::Transform::LinearDepthFromNDC(g_depth[addr], g_frame.CameraNear);
			if (neighborLinearDepth == FLT_MAX)
				continue;
			
			float2 neighborUV = (addr + 0.5) * rcpRenderDim;
			if (!GeometryTest(neighborLinearDepth, neighborUV, normal, posW, linearDepth))
				continue;
			
			float3 neighborColor = g_input[addr].rgb;
			float neighborLum = Math::Color::LuminanceFromLinearRGB(neighborColor);

			if (neighborLum < minLum)
			{
				minLum = neighborLum;
				minColor = neighborColor;
			}
			else if (neighborLum > maxLum)
			{
				maxLum = neighborLum;
				maxColor = neighborColor;
			}
		}
	}
	
	float3 ret = currLum < minLum ? minColor : (currLum > maxLum ? maxColor : currColor);
	return ret;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(FIREFLY_FILTER_THREAD_GROUP_DIM_X, FIREFLY_FILTER_THREAD_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint Gidx : SV_GroupIndex)
{
	if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	
	Texture2D<float4> g_composited= ResourceDescriptorHeap[g_local.CompositedSRVDescHeapIdx];
	RWTexture2D<float4> g_filtered = ResourceDescriptorHeap[g_local.FilteredUAVDescHeapIdx];
	float4 colorCoC = g_local.DoFIsON ? g_composited[DTid.xy].rgba : float4(g_composited[DTid.xy].rgb, 0);
	
	if (depth == 0.0)
	{
		g_filtered[DTid.xy].rgba = colorCoC;
		return;
	}

	const float3 posW = Math::Transform::WorldPosFromScreenSpace(DTid.xy,
		uint2(g_frame.RenderWidth, g_frame.RenderHeight),
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[DTid.xy]);

	colorCoC.rgb = FilterFirefly(g_composited, colorCoC.rgb, DTid.xy, GTid.xy, linearDepth, normal, posW);
	g_filtered[DTid.xy] = colorCoC;
}