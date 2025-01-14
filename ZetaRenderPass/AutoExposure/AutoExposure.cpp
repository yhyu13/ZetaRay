#include "AutoExposure.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// AutoExposure
//--------------------------------------------------------------------------------------

AutoExposure::AutoExposure() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
}

AutoExposure::~AutoExposure() noexcept
{
	Reset();
}

void AutoExposure::Init(MODE m) noexcept
{
	// frame constants
	m_rootSig.InitAsCBV(0,						// root idx
		0,										// register num
		0,										// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, 
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

	// root constants
	m_rootSig.InitAsConstants(1,				// root idx
		NUM_CONSTS,								// num DWORDs
		1,										// register
		0);										// register space
		
	m_rootSig.InitAsBufferUAV(2,				// root idx
		0,										// register num
		0,										// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	s_rpObjs.Init("AutoExposure", m_rootSig, 0, nullptr, flags);
	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);

	m_minLum = DefaultParamVals::MinLum;
	m_maxLum = DefaultParamVals::MaxLum;
	m_clampLum = DefaultParamVals::ClampLum;
	m_eps = DefaultParamVals::EPS;
	m_lumExp = DefaultParamVals::LumMappingExp;
	m_adaptationRate = DefaultParamVals::AdaptationRate;
	m_lowerPercentile = DefaultParamVals::LowerPercentile;
	m_upperPercentile = DefaultParamVals::UpperPercentile;

	SwitchMode(m);
}

void AutoExposure::Reset() noexcept
{
	if (m_psos[0] || m_psos[1])
	{
		s_rpObjs.Clear();
		m_exposure.Reset();
		m_downsampledLogLumMip5.Reset();
		m_counter.Reset();
		m_hist.Reset();
	}
}

void AutoExposure::OnWindowResized() noexcept
{
	if (m_mode == MODE::DOWNSAMPLE)
	{
		auto& renderer = App::GetRenderer();
		const uint32_t renderWidth = renderer.GetRenderWidth();
		const uint32_t renderHeight = renderer.GetRenderHeight();

		m_mip5Width = renderWidth >> 6;
		m_mip5Height = renderHeight >> 6;

		m_downsampledLogLumMip5 = renderer.GetGpuMemory().GetTexture2D("SpdMip5",
			m_mip5Width,
			m_mip5Height,
			DXGI_FORMAT_R16_FLOAT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DUAV(m_downsampledLogLumMip5, m_descTable.CPUHandle((int)DESC_TABLE::MIP5_UAV));
	}
}

void AutoExposure::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	Assert(m_inputDesc[(int)SHADER_IN_DESC::COMPOSITED] != -1, "Input descriptor hasn't been set.");

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();

	computeCmdList.PIXBeginEvent("AutoExposure");
	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "AutoExposure");

	if (m_mode == MODE::DOWNSAMPLE)
	{
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, 64);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, 64);
		const uint32_t mipLevels = (uint32_t)log2f((float)Math::Max(w, h));

		cbAutoExposureDownsample cb;
		cb.InputDescHeapIdx = m_inputDesc[(int)SHADER_IN_DESC::COMPOSITED];
		cb.MipLevels = (uint32_t)log2f((float)Math::Max(w, h));
		cb.NumThreadGroupsX = dispatchDimX;
		cb.NumThreadGroupsY = dispatchDimY;
		cb.Mip5DimX = (uint16_t)m_mip5Width;
		cb.Mip5DimY = (uint16_t)m_mip5Height;
		cb.OutputMip5DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::MIP5_UAV);
		cb.OutputLastMipDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::EXPOSURE_UAV);
		cb.ClampLum = m_clampLum;
		cb.MinLum = m_minLum;
		cb.MaxLum = m_maxLum;

		m_rootSig.SetRootUAV(2, m_counter.GetGpuVA());
		m_rootSig.SetRootConstants(0, sizeof(cbAutoExposureDownsample) / sizeof(DWORD), &cb);
		m_rootSig.End(computeCmdList);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DOWNSAMPLE]);
		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
	}
	else
	{
		cbAutoExposureHist cb;
		cb.Eps = m_eps;
		cb.MinLum = m_minLum;
		cb.LumRange = m_maxLum - m_minLum;
		cb.InputDescHeapIdx = m_inputDesc[(int)SHADER_IN_DESC::COMPOSITED];
		cb.ExposureDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::EXPOSURE_UAV);
		cb.LumMappingExp = m_lumExp;
		cb.AdaptationRate = m_adaptationRate;
		cb.LowerPercentile = m_lowerPercentile;
		cb.UpperPercentile = m_upperPercentile;
		
		m_rootSig.SetRootUAV(2, m_hist.GetGpuVA());
		m_rootSig.SetRootConstants(0, sizeof(cbAutoExposureHist) / sizeof(DWORD), &cb);
		m_rootSig.End(computeCmdList);

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, THREAD_GROUP_SIZE_HIST_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, THREAD_GROUP_SIZE_HIST_Y);

		computeCmdList.ResourceBarrier(m_hist.GetResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		computeCmdList.CopyBufferRegion(m_hist.GetResource(), 0, m_zeroBuffer.GetResource(), 0, HIST_BIN_COUNT * sizeof(uint32_t));
		computeCmdList.ResourceBarrier(m_hist.GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::HISTOGRAM]);
		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		auto uavBarrier = Direct3DHelper::UAVBarrier(m_hist.GetResource());
		computeCmdList.UAVBarrier(1, &uavBarrier);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::EXPECTED_VALUE]);
		computeCmdList.Dispatch(1, 1, 1);
	}

	computeCmdList.PIXEndEvent();
	
	// record the timestamp after execution
	gpuTimer.EndQuery(computeCmdList, queryIdx);
}

void AutoExposure::CreateDownsampleResources() noexcept
{
	auto& renderer = App::GetRenderer();

	m_exposure = renderer.GetGpuMemory().GetTexture2D("Exposure", 
		1, 
		1, 
		DXGI_FORMAT_R32G32_FLOAT,
		D3D12_RESOURCE_STATE_COMMON, 
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS | TEXTURE_FLAGS::INIT_TO_ZERO);

	const uint32_t renderWidth = renderer.GetRenderWidth();
	const uint32_t renderHeight = renderer.GetRenderHeight();

	m_mip5Width = renderWidth >> 6;
	m_mip5Height = renderHeight >> 6;

	m_downsampledLogLumMip5 = renderer.GetGpuMemory().GetTexture2D("SpdMip5", 
		m_mip5Width,
		m_mip5Height,
		DXGI_FORMAT_R16_FLOAT,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	m_counter = renderer.GetGpuMemory().GetDefaultHeapBuffer("SpdCounter",
		sizeof(uint32_t),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 
		true, 
		true);

	Direct3DHelper::CreateTexture2DUAV(m_downsampledLogLumMip5, m_descTable.CPUHandle((int)DESC_TABLE::MIP5_UAV));
	Direct3DHelper::CreateTexture2DUAV(m_exposure, m_descTable.CPUHandle((int)DESC_TABLE::EXPOSURE_UAV));
}

void AutoExposure::CreateHistogramResources() noexcept
{
	auto& renderer = App::GetRenderer();

	m_hist = renderer.GetGpuMemory().GetDefaultHeapBuffer("LogLumHistogram",
		HIST_BIN_COUNT * sizeof(uint32_t),
		D3D12_RESOURCE_STATE_COMMON,
		true);

	m_exposure = renderer.GetGpuMemory().GetTexture2D("Exposure",
		1,
		1,
		DXGI_FORMAT_R32G32_FLOAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS | TEXTURE_FLAGS::INIT_TO_ZERO);

	// create a zero-initialized buffer for resetting the counter
	m_zeroBuffer = renderer.GetGpuMemory().GetDefaultHeapBuffer("Zero",
		sizeof(uint32_t),
		D3D12_RESOURCE_STATE_COMMON,
		false,
		true);

	Direct3DHelper::CreateTexture2DUAV(m_exposure, m_descTable.CPUHandle((int)DESC_TABLE::EXPOSURE_UAV));
}

void AutoExposure::SwitchMode(MODE m) noexcept
{
	if (m == MODE::DOWNSAMPLE)
	{
		m_psos[(int)SHADERS::DOWNSAMPLE] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::DOWNSAMPLE, s_rpObjs.m_rootSig.Get(), 
			COMPILED_CS[(int)SHADERS::DOWNSAMPLE]);

		CreateDownsampleResources();

		ParamVariant p1;
		p1.InitFloat("Renderer", "AutoExposure", "MinLum", fastdelegate::MakeDelegate(this, &AutoExposure::MinLumCallback),
			DefaultParamVals::MinLum, 1e-4f, 0.5f, 1e-3f);
		App::AddParam(p1);

		ParamVariant p2;
		p2.InitFloat("Renderer", "AutoExposure", "MaxLum", fastdelegate::MakeDelegate(this, &AutoExposure::MaxLumCallback),
			DefaultParamVals::MaxLum, 0.5f, 16.0f, 0.1f);
		App::AddParam(p2);

		ParamVariant p3;
		p3.InitBool("Renderer", "AutoExposure", "ClampLum", fastdelegate::MakeDelegate(this, &AutoExposure::LumClampingCallback),
			DefaultParamVals::ClampLum);
		App::AddParam(p3);
	}
	else
	{
		ParamVariant p1;
		p1.InitFloat("Renderer", "AutoExposure", "MinLum", fastdelegate::MakeDelegate(this, &AutoExposure::MinLumCallback),
			DefaultParamVals::MinLum, 1e-4f, 0.5f, 1e-3f);
		App::AddParam(p1);

		ParamVariant p2;
		p2.InitFloat("Renderer", "AutoExposure", "MaxLum", fastdelegate::MakeDelegate(this, &AutoExposure::MaxLumCallback),
			DefaultParamVals::MaxLum, 0.5f, 16.0f, 0.1f);
		App::AddParam(p2);

		ParamVariant p5;
		p5.InitFloat("Renderer", "AutoExposure", "LowerPercentile", fastdelegate::MakeDelegate(this, &AutoExposure::LowerPercentileCallback),
			DefaultParamVals::LowerPercentile, 0.0f, 0.3f, 0.01f);
		App::AddParam(p5);

		ParamVariant p6;
		p6.InitFloat("Renderer", "AutoExposure", "UpperPercentile", fastdelegate::MakeDelegate(this, &AutoExposure::UpperPercentileCallback),
			DefaultParamVals::UpperPercentile, 0.6f, 1.0f, 0.01f);
		App::AddParam(p6);

		ParamVariant p3;
		p3.InitFloat("Renderer", "AutoExposure", "LumMappingExp", fastdelegate::MakeDelegate(this, &AutoExposure::LumMappingExpCallback),
			DefaultParamVals::LumMappingExp, 1e-1f, 1.0f, 1e-2f);
		App::AddParam(p3);			
			
		ParamVariant p4;
		p4.InitFloat("Renderer", "AutoExposure", "AdaptationRate", fastdelegate::MakeDelegate(this, &AutoExposure::AdaptationRateCallback),
			DefaultParamVals::AdaptationRate, 1e-3f, 5.0f, 1e-1f);
		App::AddParam(p4);

		m_psos[(int)SHADERS::HISTOGRAM] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::HISTOGRAM, s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[(int)SHADERS::HISTOGRAM]);
		m_psos[(int)SHADERS::EXPECTED_VALUE] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::EXPECTED_VALUE, s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[(int)SHADERS::EXPECTED_VALUE]);

		CreateHistogramResources();
	}

	m_mode = m;
}

void AutoExposure::MinLumCallback(const Support::ParamVariant& p) noexcept
{
	m_minLum = Math::Min(p.GetFloat().m_val, m_maxLum);
}

void AutoExposure::MaxLumCallback(const Support::ParamVariant& p) noexcept
{
	m_maxLum = Math::Max(p.GetFloat().m_val, m_minLum);
}

void AutoExposure::LumClampingCallback(const Support::ParamVariant& p) noexcept
{
	m_clampLum = p.GetBool();
}

void AutoExposure::LumMappingExpCallback(const Support::ParamVariant& p) noexcept
{
	m_lumExp = p.GetFloat().m_val;
}

void AutoExposure::AdaptationRateCallback(const Support::ParamVariant& p) noexcept
{
	m_adaptationRate = p.GetFloat().m_val;
}

void AutoExposure::LowerPercentileCallback(const Support::ParamVariant& p) noexcept
{
	m_lowerPercentile = p.GetFloat().m_val;
}

void AutoExposure::UpperPercentileCallback(const Support::ParamVariant& p) noexcept
{
	m_upperPercentile = p.GetFloat().m_val;
}


