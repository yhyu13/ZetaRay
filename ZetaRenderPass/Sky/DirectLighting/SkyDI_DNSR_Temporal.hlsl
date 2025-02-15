#include "SkyDI_Reservoir.hlsli"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/BRDF.hlsli"
#include "../../Common/StaticTextureSamplers.hlsli"
#include "../../Common/Common.hlsli"
#include "../../Common/VolumetricLighting.hlsli"

#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.005f
#define VIEW_ANGLE_EXP 0.15f
#define ROUGHNESS_EXP_SCALE 0.95f

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_SkyDI_DNSR_Temporal> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

// output range is [0, +inf)
float Parallax(float3 currPos, float3 prevPos, float3 currCamPos, float3 prevCamPos)
{
	float3 v1 = normalize(currPos - currCamPos);
	float3 v2 = normalize(prevPos - prevCamPos);
	
	// theta is the angle between v1 & v2
	float cosTheta = saturate(dot(v1, v2));
	float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
	
	float p = sinTheta / max(1e-6f, cosTheta);
	p = pow(p, 0.2f);
	p *= p >= 1e-4;
	
	return p;
}

float Reactivity(float roughness, float ndotwo, float parallax)
{
	float a = saturate(1.0f - ndotwo);
	a = pow(a, VIEW_ANGLE_EXP);
	float b = 1.1f + roughness * roughness;
	float parallaxSensitivity = (b + a) / (b - a); // range in [1, +inf)
	
	// exponetially less temporal accumulation as roughness goes to 0
	float powScale = 1.0f + parallax * parallaxSensitivity;
	float f = 1.0f - exp2(-200.0f * roughness * roughness);
	
	// exponentially higher reactivity depending on roughness, parallax and its sensitivity
	f *= pow(roughness, ROUGHNESS_EXP_SCALE * powScale);
		
	return 1 - f;
}

float4 GeometryTest(float4 prevDepths, float2 prevUVs[4], float3 currNormal, float3 currPos, float linearDepth)
{
	float3 prevPos[4];
	prevPos[0] = Math::Transform::WorldPosFromUV(prevUVs[0], prevDepths.x, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[1] = Math::Transform::WorldPosFromUV(prevUVs[1], prevDepths.y, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[2] = Math::Transform::WorldPosFromUV(prevUVs[2], prevDepths.z, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[3] = Math::Transform::WorldPosFromUV(prevUVs[3], prevDepths.w, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	
	float4 planeDist = float4(dot(currNormal, prevPos[0] - currPos),
		dot(currNormal, prevPos[1] - currPos),
		dot(currNormal, prevPos[2] - currPos),
		dot(currNormal, prevPos[3] - currPos));
	
	float4 weights = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;
	
	return weights;
}

float GeometryTest(float prevDepth, float2 prevUV, float3 currNormal, float3 currPos, float linearDepth)
{
	float3 prevPos = Math::Transform::WorldPosFromUV(prevUV, prevDepth, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	
	float planeDist = dot(currNormal, prevPos - currPos);
	float weight = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;
	
	return weight;
}

float4 NormalWeight(float3 prevNormals[4], float3 currNormal, float roughness)
{
	float4 cosTheta = saturate(float4(dot(currNormal, prevNormals[0]),
		dot(currNormal, prevNormals[1]),
		dot(currNormal, prevNormals[2]),
		dot(currNormal, prevNormals[3])));
	
	float normalExp = lerp(16, 128, 1 - roughness);
	float4 weight = pow(cosTheta, normalExp);
	
	return weight;
}

// helps with high frequency roughness textures
float4 RoughnessWeight(float currRoughness, float4 prevRoughness)
{
	float n = currRoughness * currRoughness * 0.99f + 0.01f;
	float4 w = abs(currRoughness - prevRoughness) / n;
	w = saturate(1.0f - w);
	bool4 b1 = prevRoughness < g_local.MinRoughnessResample;
	bool4 b2 = currRoughness < g_local.MinRoughnessResample;
	// don't take roughness into account when there's been a sudden change
	w = select(w, 1.0.xxxx, b1 ^ b2);

	return w;
}

void SampleTemporalCache_Bilinear(uint2 DTid, float3 currPos, float3 currNormal, float linearDepth, float2 currUV, float2 prevUV,
	BRDF::SurfaceInteraction surface, out float tspp, out float3 color)
{
	color = 0.0.xxx;
	tspp = 0;
	
	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 screenDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 f = prevUV * screenDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / screenDim;
			
	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	prevDepths = Math::Transform::LinearDepthFromNDC(prevDepths, g_frame.CameraNear);
	
	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	const float4 geoWeights = GeometryTest(prevDepths, prevUVs, currNormal, currPos, linearDepth);

	// weight must be zero for out-of-bound samples
	const float4 isInBounds = float4(Math::IsWithinBoundsExc(topLeft, screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(1, 0), screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(0, 1), screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(1, 1), screenDim));

	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	float4 weights = geoWeights * bilinearWeights * isInBounds;
	// zero out samples with very low weights to avoid bright spots
	weights *= weights > 1e-3f;
	const float weightSum = dot(1.0f, weights);

	if (1e-4f < weightSum)
	{
		// uniformly distribute the weight over the valid samples
		weights /= weightSum;

		// tspp
		Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDiffuseDescHeapIdx];
		uint4 histTspp = (uint4) g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
		
		histTspp = max(1, histTspp);
		tspp = round(dot(histTspp, weights));
		
		if (tspp > 0)
		{
			// color
			float3 colorHistSamples[4];
			const float4 histR = g_prevTemporalCache.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
			const float4 histG = g_prevTemporalCache.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
			const float4 histB = g_prevTemporalCache.GatherBlue(g_samPointClamp, topLeftTexelUV).wzxy;
			
			colorHistSamples[0] = float3(histR.x, histG.x, histB.x);
			colorHistSamples[1] = float3(histR.y, histG.y, histB.y);
			colorHistSamples[2] = float3(histR.z, histG.z, histB.z);
			colorHistSamples[3] = float3(histR.w, histG.w, histB.w);

			color = colorHistSamples[0] * weights[0] +
					colorHistSamples[1] * weights[1] +
					colorHistSamples[2] * weights[2] +
					colorHistSamples[3] * weights[3];
		}
	}
}

bool SampleTemporalCache_CatmullRom(uint2 DTid, float3 currPos, float3 currNormal, float linearDepth, float2 currUV, float2 prevUV,
	out float tspp, out float3 color, out float prevLinearDepth)
{
	color = 0.0.xxx;
	tspp = 0;
	const float2 screenDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	float2 samplePos = prevUV * screenDim;
	float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
	float2 f = samplePos - texPos1;
	float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	float2 w3 = f * f * (-0.5f + 0.5f * f);

	float2 w12 = w1 + w2;
	float2 offset12 = w2 / (w1 + w2);

	float2 texPos0 = texPos1 - 1;
	float2 texPos3 = texPos1 + 2;
	float2 texPos12 = texPos1 + offset12;

	texPos0 /= screenDim;
	texPos3 /= screenDim;
	texPos12 /= screenDim;

	float2 prevUVs[5];
	prevUVs[0] = float2(texPos12.x, texPos0.y);
	prevUVs[1] = float2(texPos0.x, texPos12.y);
	prevUVs[2] = float2(texPos12.x, texPos12.y);
	prevUVs[3] = float2(texPos3.x, texPos12.y);
	prevUVs[4] = float2(texPos12.x, texPos3.y);

	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float2 prevDepths[5];
		
	[unroll]
	for (int i = 0; i < 5; i++)
	{
		prevDepths[i].x = g_prevDepth.SampleLevel(g_samLinearClamp, prevUVs[i], 0.0f);
		prevDepths[i].y = Math::Transform::LinearDepthFromNDC(prevDepths[i].x, g_frame.CameraNear);
	}
	
	// to reconstruct previous surface pos
	prevLinearDepth = prevDepths[0].x * w12.x * w0.y + prevDepths[1].x * w0.x * w12.y + prevDepths[2].x * w12.x * w12.y +
		prevDepths[3].x * w3.x * w12.y + prevDepths[4].x * w12.x * w3.y;
	prevLinearDepth = Math::Transform::LinearDepthFromNDC(prevLinearDepth, g_frame.CameraNear);
	
	float weights[5];
	
	[unroll]
	for (int j = 0; j < 5; j++)
	{
		float isInBounds = all(prevUVs[j] <= 1.0.xx) && all(prevUVs[j] >= 0.0f);
		float geoWeight = GeometryTest(prevDepths[j].y, prevUVs[j], currNormal, currPos, linearDepth);
		weights[j] = isInBounds * geoWeight;
	}

	bool allValid = weights[0] > 1e-3f;
	
	[unroll]
	for (int k = 1; k < 5; k++)
		allValid = allValid && (weights[k] > 1e-3f);
	
	if (allValid)
	{
		Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDiffuseDescHeapIdx];
		
		float4 results[5];
		results[0] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[0], 0.0f) * w12.x * w0.y;

		results[1] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[1], 0.0f) * w0.x * w12.y;
		results[2] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[2], 0.0f) * w12.x * w12.y;
		results[3] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[3], 0.0f) * w3.x * w12.y;

		results[4] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[4], 0.0f) * w12.x * w3.y;

		tspp = results[0].w * weights[0] +
			results[1].w * weights[1] +
			results[2].w * weights[2] +
			results[3].w * weights[3] +
			results[4].w * weights[4];
		
		tspp = max(1, tspp);
		color = results[0].rgb + results[1].rgb + results[2].rgb + results[3].rgb + results[4].rgb;
		
		return true;
	}
	
	return false;
}

void TemporalAccumulation_Diffuse(uint2 DTid, float2 currUV, float3 posW, float3 normal, float linearDepth, float metalness, 
	float roughness, BRDF::SurfaceInteraction surface, DIReservoir r, out float prevSurfaceLinearDepth, out float2 prevSurfaceUV)
{
	prevSurfaceLinearDepth = 0.0f;
	
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const float2 motionVec = g_motionVector[DTid.xy];
	prevSurfaceUV = currUV - motionVec;
	const bool motionVecValid = all(prevSurfaceUV >= 0.0f) && all(prevSurfaceUV <= 1.0f);

	if (metalness >= MIN_METALNESS_METAL)
	{
		GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
		float z = g_prevDepth.SampleLevel(g_samLinearClamp, prevSurfaceUV, 0.0f);	
		prevSurfaceLinearDepth = motionVecValid ? Math::Transform::LinearDepthFromNDC(z, g_frame.CameraNear) : 0.0f;
		
		return;
	}
		
	float demodulatedDiffuseReflectance = (1 - metalness);
	float3 f = (1 - surface.F) * demodulatedDiffuseReflectance * saturate(dot(r.wi, normal)) * ONE_OVER_PI;
	float3 signal = r.Li * f * r.W;
	
	float tspp = 0;
	float3 color = 0.0.xxx;
	
	if (g_local.IsTemporalCacheValid && motionVecValid)
	{
		// try to use Catmull-Rom interpolation first
		bool success = SampleTemporalCache_CatmullRom(DTid.xy, posW, normal, linearDepth, currUV, prevSurfaceUV, 
			tspp, color, prevSurfaceLinearDepth);
		
		// if it failed, then resample history using a bilinear filter with custom weights
		if (!success)
			SampleTemporalCache_Bilinear(DTid.xy, posW, normal, linearDepth, currUV, prevSurfaceUV, surface, tspp, color);
	}
	
	float3 currColor = dot(color, 1) <= 1e-5 ? signal : lerp(color, signal, 1.0f / (1.0f + tspp));
	
	float maxTspp = roughness < g_local.MinRoughnessResample ? 0 : g_local.MaxTSPP_Diffuse;
	tspp = min(tspp + 1, maxTspp);
	
	RWTexture2D<float4> g_currTemporalCache_Diffuse = ResourceDescriptorHeap[g_local.CurrTemporalCacheDiffuseDescHeapIdx];
	g_currTemporalCache_Diffuse[DTid] = float4(currColor, tspp);
}

void SampleTemporalCache_Virtual(uint2 DTid, float3 posW, float3 normal, float linearDepth, float2 uv, float metalness, float roughness,
	BRDF::SurfaceInteraction surface, float3 wi, float2 prevSurfaceUV, out float3 color, out float tspp)
{
	color = 0.0.xxx;
	tspp = 0.0;

	// reverse reproject using virtual motion
	float3 posPlanet = posW;
	posPlanet.y += g_frame.PlanetRadius;
	float rayT = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posPlanet, wi);
	float2 prevUV = prevSurfaceUV;
	
	if (roughness < g_local.MinRoughnessResample)
	{
		GBUFFER_CURVATURE g_curvature = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::CURVATURE];
		const float localCurvature = g_curvature[DTid.xy];

		prevUV = SkyDI_Util::VirtualMotionReproject(posW, roughness, surface, rayT, localCurvature, linearDepth,
			g_frame.TanHalfFOV, g_frame.PrevViewProj);
	}
	
	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 f = prevUV * renderDim;
	const float2 topLeft = floor(f - 0.5f);
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / renderDim;

	// screen-bounds check
	float4 weights = float4(Math::IsWithinBoundsExc(topLeft, renderDim),
							Math::IsWithinBoundsExc(topLeft + float2(1, 0), renderDim),
							Math::IsWithinBoundsExc(topLeft + float2(0, 1), renderDim),
							Math::IsWithinBoundsExc(topLeft + float2(1, 1), renderDim));

	if (dot(1, weights) == 0)
		return;
			
	// geometry weight
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepthsNDC = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	float4 prevLinearDepths = Math::Transform::LinearDepthFromNDC(prevDepthsNDC, g_frame.CameraNear);
	
	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	weights *= GeometryTest(prevLinearDepths, prevUVs, normal, posW, linearDepth);
	
	// normal weight
	GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];

	// w (0, 0)		z (1,0)
	// x (0, 1)		y (1, 1)
	const float4 prevNormalsXEncoded = g_prevNormal.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	const float4 prevNormalsYEncoded = g_prevNormal.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	
	float3 prevNormals[4];
	prevNormals[0] = Math::Encoding::DecodeUnitNormal(float2(prevNormalsXEncoded.x, prevNormalsYEncoded.x));
	prevNormals[1] = Math::Encoding::DecodeUnitNormal(float2(prevNormalsXEncoded.y, prevNormalsYEncoded.y));
	prevNormals[2] = Math::Encoding::DecodeUnitNormal(float2(prevNormalsXEncoded.z, prevNormalsYEncoded.z));
	prevNormals[3] = Math::Encoding::DecodeUnitNormal(float2(prevNormalsXEncoded.w, prevNormalsYEncoded.w));
	weights *= NormalWeight(prevNormals, normal, roughness);

	// roughness weight
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float4 prevRoughness = g_metalnessRoughness.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	weights *= RoughnessWeight(roughness, prevRoughness);

	// metalness weight
	const float4 prevMetalness= g_metalnessRoughness.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	weights *= prevMetalness == metalness;
	
	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	weights *= bilinearWeights;
	weights *= weights > 1e-2f;
	const float weightSum = dot(1.0f, weights);

	if (weightSum < 1e-3f)
		return;
	
	// uniformly distribute the weight over the nonzero samples
	weights /= weightSum;

	// tspp
	Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheSpecularDescHeapIdx];
	float4 histTspp = g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
	histTspp = max(1, histTspp);
	tspp = round(dot(histTspp, weights));
		
	if (tspp > 0)
	{
		float3 histColor[4];
		const float4 histR = g_prevTemporalCache.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
		const float4 histG = g_prevTemporalCache.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
		const float4 histB = g_prevTemporalCache.GatherBlue(g_samPointClamp, topLeftTexelUV).wzxy;
			
		histColor[0] = float3(histR.x, histG.x, histB.x);
		histColor[1] = float3(histR.y, histG.y, histB.y);
		histColor[2] = float3(histR.z, histG.z, histB.z);
		histColor[3] = float3(histR.w, histG.w, histB.w);

		color = histColor[0] * weights[0] +
				histColor[1] * weights[1] +
				histColor[2] * weights[2] +
				histColor[3] * weights[3];
	}
}

void TemporalAccumulation_Specular(uint2 DTid, float2 currUV, float3 posW, float3 normal, float linearDepth, float metalness, 
	float roughness, BRDF::SurfaceInteraction surface, DIReservoir r, float prevSurfaceLinearDepth, float2 prevSurfaceUV)
{
	float3 color = 0.0.xxx;
	float tspp = 0;
	if (g_local.IsTemporalCacheValid)
	{
		SampleTemporalCache_Virtual(DTid.xy, posW, normal, linearDepth, currUV, metalness, roughness, surface,
			r.wi, prevSurfaceUV, color, tspp);
	}
	
	const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
	const float3 prevSurfacePosW = Math::Transform::WorldPosFromUV(prevSurfaceUV,
		prevSurfaceLinearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);

	const float parallax = Parallax(posW, prevSurfacePosW, g_frame.CameraPos, prevCameraPos);
	float reactivity = Reactivity(roughness, surface.whdotwo, parallax);
	float minTspp = metalness > MIN_METALNESS_METAL ? 0 : 1;
	tspp = clamp((1 - reactivity) * g_local.MaxTSPP_Specular, minTspp, g_local.MaxTSPP_Specular);

	float3 f = BRDF::SpecularBRDFGGXSmith(surface);
	float3 signal = r.Li * f * r.W;
	float3 currColor = dot(color, 1) <= 1e-5 ? signal : lerp(color, signal, 1.0f / (1.0f + tspp));
	
	RWTexture2D<float4> g_currTemporalCache_Specular = ResourceDescriptorHeap[g_local.CurrTemporalCacheSpecularDescHeapIdx];
	g_currTemporalCache_Specular[DTid.xy].xyzw = float4(currColor, tspp);
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(SKY_DI_DNSR_TEMPORAL_GROUP_DIM_X, SKY_DI_DNSR_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
	if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
		return;
	
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_currDepth[DTid.xy];

	// skip sky pixels
	if (depth == 0.0)
		return;

	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float2 mr = g_metalnessRoughness[DTid.xy];

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[DTid.xy]);

	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const float2 currUV = (DTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromUV(currUV,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float3 baseColor = g_baseColor[DTid.xy].rgb;

	const float3 wo = normalize(g_frame.CameraPos - posW);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal, mr.y, wo);

	DIReservoir r = SkyDI_Util::PartialReadReservoir_Shading(DTid.xy, g_local.InputReservoir_A_DescHeapIdx);
	surface.InitComplete(r.wi, baseColor, mr.x, normal);
	
	if (!g_local.IsTemporalCacheValid || !g_local.Denoise)
	{
		RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];
		float3 f = BRDF::ComputeSurfaceBRDF(surface);
		g_final[DTid.xy].rgb = r.Li * r.W * f;
		
		return;
	}

	float prevSurfaceLinearDepth;
	float2 prevSurfaceUV;
	TemporalAccumulation_Diffuse(DTid.xy, currUV, posW, normal, linearDepth, mr.x, mr.y, surface, r,
		prevSurfaceLinearDepth, prevSurfaceUV);
	TemporalAccumulation_Specular(DTid.xy, currUV, posW, normal, linearDepth, mr.x, mr.y, surface, r,
		prevSurfaceLinearDepth, prevSurfaceUV);
}