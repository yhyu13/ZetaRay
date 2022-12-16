#ifndef SUN_SHADOW_COMMON_H
#define SUN_SHADOW_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define SUN_SHADOW_THREAD_GROUP_SIZE_X 8
#define SUN_SHADOW_THREAD_GROUP_SIZE_Y 4

#define DNSR_TEMPORAL_THREAD_GROUP_SIZE_X 8
#define DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y 8

#define DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_X 8
#define DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y 8

struct cbSunShadow
{
	uint32_t OutShadowMaskDescHeapIdx;
};

struct cbFFX_DNSR_Temporal
{
	uint32_t ShadowMaskDescHeapIdx;
	uint32_t CurrMetadataDescHeapIdx;
	uint32_t CurrMomentsHeapIdx;
	uint32_t PrevTemporalCacheHeapIdx;
	uint32_t CurrTemporalCacheHeapIdx;
	uint16_t NumShadowMaskThreadGroupsX;
	uint16_t NumShadowMaskThreadGroupsY;

	uint16_t IsTemporalValid;
	uint16_t pad;
};

struct cbFFX_DNSR_Spatial
{
	uint32_t CurrMetadataDescHeapIdx;
	uint32_t CurrTemporalCacheHeapIdx;
	uint32_t CurrMomentsHeapIdx;
	float DepthSigma;
	uint16_t PassNum;
	uint16_t StepSize;
};

#endif
