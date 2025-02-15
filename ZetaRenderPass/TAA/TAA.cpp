#include "TAA.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// TAA
//--------------------------------------------------------------------------------------

TAA::TAA() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,								// root idx
		0,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

	// root constants
	m_rootSig.InitAsConstants(1,		// root idx
		sizeof(cbTAA) / sizeof(DWORD),	// num DWORDs
		1,								// register
		0);								// register space
}

TAA::~TAA() noexcept
{
	Reset();
}

void TAA::Init() noexcept
{
	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("TAA", m_rootSig, samplers.size(), samplers.data(), flags);

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateResources();

	m_localCB.BlendWeight = DefaultParamVals::BlendWeight;

	ParamVariant blendWeight;
	blendWeight.InitFloat("Renderer", "TAA", "BlendWeight", fastdelegate::MakeDelegate(this, &TAA::BlendWeightCallback),
		DefaultParamVals::BlendWeight,			// val	
		0.0f,									// min
		1.0f,									// max
		0.1f);									// step
	App::AddParam(blendWeight);

	m_isTemporalTexValid = false;
	//App::AddShaderReloadHandler("TAA", fastdelegate::MakeDelegate(this, &TAA::ReloadShader));
}

void TAA::Reset() noexcept
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();
		App::RemoveParam("Renderer", "TAA", "BlendWeight");
		App::RemoveShaderReloadHandler("TAA");

		m_antiAliased[0].Reset();
		m_antiAliased[1].Reset();

		m_descTable.Reset();
		m_pso = nullptr;
	}
}

void TAA::OnWindowResized() noexcept
{
	CreateResources();
	m_isTemporalTexValid = false;
}

void TAA::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const int outIdx = renderer.GlobaIdxForDoubleBufferedResources();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();

	Assert(m_inputDesc[(int)SHADER_IN_DESC::SIGNAL] > 0, "Input SRV hasn't been set.");
	m_localCB.InputDescHeapIdx = m_inputDesc[(int)SHADER_IN_DESC::SIGNAL];
	m_localCB.PrevOutputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex() + (outIdx == 0 ? (int)DESC_TABLE::TEX_A_SRV : (int)DESC_TABLE::TEX_B_SRV);
	m_localCB.CurrOutputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex() + (outIdx == 0 ? (int)DESC_TABLE::TEX_B_UAV : (int)DESC_TABLE::TEX_A_UAV);
	m_localCB.TemporalIsValid = m_isTemporalTexValid;

	computeCmdList.PIXBeginEvent("TAA");

	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "TAA");

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	computeCmdList.SetPipelineState(m_pso);

	m_rootSig.SetRootConstants(0, sizeof(cbTAA) / sizeof(DWORD), &m_localCB);
	m_rootSig.End(computeCmdList);

	computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(w, TAA_THREAD_GROUP_SIZE_X), 
		(uint32_t)CeilUnsignedIntDiv(h, TAA_THREAD_GROUP_SIZE_Y), 1);

	computeCmdList.PIXEndEvent();

	// record the timestamp after execution
	gpuTimer.EndQuery(computeCmdList, queryIdx);

	m_isTemporalTexValid = true;
}

void TAA::CreateResources() noexcept
{
	auto& renderer = App::GetRenderer();

	m_antiAliased[0] = renderer.GetGpuMemory().GetTexture2D("TAA_A",
		renderer.GetRenderWidth(), renderer.GetRenderHeight(),
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	m_antiAliased[1] = renderer.GetGpuMemory().GetTexture2D("TAA_B",
		renderer.GetRenderWidth(), renderer.GetRenderHeight(),
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	// SRVs
	Direct3DHelper::CreateTexture2DSRV(m_antiAliased[0], m_descTable.CPUHandle((int)DESC_TABLE::TEX_A_SRV));
	Direct3DHelper::CreateTexture2DSRV(m_antiAliased[1], m_descTable.CPUHandle((int)DESC_TABLE::TEX_B_SRV));

	// UAVs
	Direct3DHelper::CreateTexture2DUAV(m_antiAliased[0], m_descTable.CPUHandle((int)DESC_TABLE::TEX_A_UAV));
	Direct3DHelper::CreateTexture2DUAV(m_antiAliased[1], m_descTable.CPUHandle((int)DESC_TABLE::TEX_B_UAV));
}

void TAA::BlendWeightCallback(const ParamVariant& p) noexcept
{
	m_localCB.BlendWeight = p.GetFloat().m_val;
}

void TAA::ReloadShader() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "TAA\\TAA.hlsl", true);
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);
}
