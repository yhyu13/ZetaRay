#pragma once

#include "Device.h"

namespace ZetaRay::Core::Constants
{
	static constexpr int NUM_BACK_BUFFERS = 3;
	static constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	static constexpr DXGI_FORMAT DEPTH_BUFFER_FORMAT = DXGI_FORMAT_D32_FLOAT;

	static constexpr int NUM_CBV_SRV_UAV_DESC_HEAP_GPU_DESCRIPTORS = 8192;
	static constexpr int NUM_CBV_SRV_UAV_DESC_HEAP_CPU_DESCRIPTORS = 128;
	static constexpr int NUM_RTV_DESC_HEAP_DESCRIPTORS = 32;
	static constexpr int NUM_DSV_DESC_HEAP_DESCRIPTORS = 16;
	static constexpr int MAX_SWAPCHAIN_FRAME_LATENCY = 2;

	static constexpr D3D12_RESOURCE_STATES VALID_BUFFER_STATES =
		D3D12_RESOURCE_STATE_COMMON |
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
		D3D12_RESOURCE_STATE_INDEX_BUFFER |
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_COPY_DEST |
		D3D12_RESOURCE_STATE_COPY_SOURCE |
		D3D12_RESOURCE_STATE_RESOLVE_DEST |
		D3D12_RESOURCE_STATE_RESOLVE_SOURCE |
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE |
		D3D12_RESOURCE_STATE_PREDICATION;

	static constexpr D3D12_RESOURCE_STATES READ_STATES =
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
		D3D12_RESOURCE_STATE_INDEX_BUFFER |
		D3D12_RESOURCE_STATE_DEPTH_READ |
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

	static constexpr D3D12_RESOURCE_STATES WRITE_STATES =
		D3D12_RESOURCE_STATE_RENDER_TARGET |
		D3D12_RESOURCE_STATE_DEPTH_WRITE |
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

	static constexpr D3D12_RESOURCE_STATES VALID_COMPUTE_QUEUE_STATES =
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_COPY_DEST |
		D3D12_RESOURCE_STATE_COPY_SOURCE |
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

	static constexpr D3D12_RESOURCE_STATES INVALID_COMPUTE_STATES =
		~VALID_COMPUTE_QUEUE_STATES;
}