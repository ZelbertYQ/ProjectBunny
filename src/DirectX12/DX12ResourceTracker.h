#pragma once

#include <Windows.h>
#include <d3d12.h>

void DX12HookResourceMetadata(ID3D12Device *device);
void DX12RecordPsoRootSignature(
	UINT64 psoIndex, const char *kind, ID3D12PipelineState *pipelineState,
	ID3D12RootSignature *rootSignature);
void DX12DumpResourceMetadata(const wchar_t *dir);
