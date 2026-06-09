#pragma once

#include <d3d12.h>

struct DX12PsoShaderInfo
{
	UINT64 psoIndex;
	bool hasVS;
	bool hasPS;
	bool hasCS;
	UINT64 vs;
	UINT64 ps;
	UINT64 cs;
};

void DX12RecordGraphicsPipelineState(
	ID3D12PipelineState *pipelineState, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc);
void DX12RecordComputePipelineState(
	ID3D12PipelineState *pipelineState, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc);
void DX12RecordPipelineStateStream(
	ID3D12PipelineState *pipelineState, const D3D12_PIPELINE_STATE_STREAM_DESC *desc);
bool DX12GetPipelineStateShaderInfo(ID3D12PipelineState *pipelineState, DX12PsoShaderInfo *info);
void DX12DumpCachedShaders();
