#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <string>
#include <vector>

#include "DX12ResourceTracker.h"

struct DX12FrameResourceBinding
{
	UINT64 psoIndex = 0;
	std::string bindSpace;
	UINT rootParameterIndex = 0;
	ID3D12DescriptorHeap *heap = nullptr;
	UINT heapType = 0;
	UINT64 descriptorIndex = 0;
	UINT64 gpuHandle = 0;
	SIZE_T cpuHandle = 0;
	UINT64 heapGpuStart = 0;
	DX12DescriptorSummary descriptor;
	bool hasDescriptor = false;
};

void DX12BindingRegisterCommandList(ID3D12GraphicsCommandList *commandList);
void DX12BindingResetCommandList(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *initialState);
void DX12BindingSetPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState);
void DX12BindingRecordStateEvent(ID3D12GraphicsCommandList *commandList, const char *kind);
void DX12BindingSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList, UINT count,
	ID3D12DescriptorHeap *const *heaps);
void DX12BindingSetGraphicsRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor);
void DX12BindingSetComputeRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor);
void DX12BindingRecordDraw(ID3D12GraphicsCommandList *commandList, const char *kind);
void DX12BindingRecordDispatch(ID3D12GraphicsCommandList *commandList);
void DX12BindingBeginFrame();
void DX12GetCurrentFrameResourceBindings(std::vector<DX12FrameResourceBinding> *bindings);
void DX12DumpBindingTrace(const wchar_t *dir);
