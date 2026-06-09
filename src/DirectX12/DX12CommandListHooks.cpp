#include "DX12CommandListHooks.h"

#include <d3d12.h>

#include "DX12ShaderHunt.h"
#include "DX12State.h"

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMAND_LIST)(
	ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
	ID3D12PipelineState*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMAND_LIST1)(
	ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
typedef void(STDMETHODCALLTYPE *PFN_SET_PIPELINE_STATE)(
	ID3D12GraphicsCommandList*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE *PFN_DRAW_INSTANCED)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DRAW_INDEXED_INSTANCED)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DISPATCH)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_EXECUTE_INDIRECT)(
	ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
	ID3D12Resource*, UINT64);

static PFN_CREATE_COMMAND_LIST gOrigCreateCommandList = nullptr;
static PFN_CREATE_COMMAND_LIST1 gOrigCreateCommandList1 = nullptr;
static PFN_SET_PIPELINE_STATE gOrigSetPipelineState = nullptr;
static PFN_DRAW_INSTANCED gOrigDrawInstanced = nullptr;
static PFN_DRAW_INDEXED_INSTANCED gOrigDrawIndexedInstanced = nullptr;
static PFN_DISPATCH gOrigDispatch = nullptr;
static PFN_EXECUTE_INDIRECT gOrigExecuteIndirect = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateCommandList(
	ID3D12Device *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
	ID3D12CommandAllocator *allocator, ID3D12PipelineState *initialState,
	REFIID riid, void **commandList)
{
	HRESULT hr = gOrigCreateCommandList(device, nodeMask, type, allocator, initialState, riid, commandList);
	if (SUCCEEDED(hr) && commandList && *commandList) {
		DX12Log("ID3D12Device::CreateCommandList type=%d result=0x%lx commandList=%p initialPso=%p\n",
			static_cast<int>(type), hr, *commandList, initialState);
		DX12HookCommandList(static_cast<IUnknown*>(*commandList));
		if (initialState) {
			ID3D12GraphicsCommandList *baseList = nullptr;
			if (SUCCEEDED(static_cast<IUnknown*>(*commandList)->QueryInterface(IID_PPV_ARGS(&baseList)))) {
				DX12HuntSetPipelineState(baseList, initialState);
				baseList->Release();
			}
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandList1(
	ID3D12Device4 *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
	D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **commandList)
{
	HRESULT hr = gOrigCreateCommandList1(device, nodeMask, type, flags, riid, commandList);
	if (SUCCEEDED(hr) && commandList && *commandList) {
		DX12Log("ID3D12Device4::CreateCommandList1 type=%d result=0x%lx commandList=%p\n",
			static_cast<int>(type), hr, *commandList);
		DX12HookCommandList(static_cast<IUnknown*>(*commandList));
	}
	return hr;
}

static void STDMETHODCALLTYPE HookedSetPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState)
{
	DX12HuntSetPipelineState(commandList, pipelineState);
	gOrigSetPipelineState(commandList, pipelineState);
}

static void STDMETHODCALLTYPE HookedDrawInstanced(
	ID3D12GraphicsCommandList *commandList, UINT vertexCountPerInstance, UINT instanceCount,
	UINT startVertexLocation, UINT startInstanceLocation)
{
	DX12HuntRecordDraw(commandList);
	gOrigDrawInstanced(commandList, vertexCountPerInstance, instanceCount,
		startVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(
	ID3D12GraphicsCommandList *commandList, UINT indexCountPerInstance, UINT instanceCount,
	UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation)
{
	DX12HuntRecordDraw(commandList);
	gOrigDrawIndexedInstanced(commandList, indexCountPerInstance, instanceCount,
		startIndexLocation, baseVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDispatch(
	ID3D12GraphicsCommandList *commandList, UINT threadGroupCountX,
	UINT threadGroupCountY, UINT threadGroupCountZ)
{
	DX12HuntRecordDispatch(commandList);
	gOrigDispatch(commandList, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

static void STDMETHODCALLTYPE HookedExecuteIndirect(
	ID3D12GraphicsCommandList *commandList, ID3D12CommandSignature *commandSignature,
	UINT maxCommandCount, ID3D12Resource *argumentBuffer, UINT64 argumentBufferOffset,
	ID3D12Resource *countBuffer, UINT64 countBufferOffset)
{
	DX12HuntRecordExecuteIndirect(commandList);
	gOrigExecuteIndirect(commandList, commandSignature, maxCommandCount, argumentBuffer,
		argumentBufferOffset, countBuffer, countBufferOffset);
}

void DX12HookCommandList(IUnknown *commandList)
{
	if (!commandList)
		return;

	ID3D12GraphicsCommandList *baseList = nullptr;
	if (FAILED(commandList->QueryInterface(IID_PPV_ARGS(&baseList))))
		return;

	void **vtable = *reinterpret_cast<void***>(baseList);
	if (vtable) {
		DX12HookFunction(reinterpret_cast<void**>(&gOrigSetPipelineState),
			vtable[26], HookedSetPipelineState, "ID3D12GraphicsCommandList::SetPipelineState");
		// Draw/Dispatch hooks are intentionally disabled for now. Some games are sensitive
		// to intercepting these very hot command-list methods; SetPipelineState is enough
		// to build the first stable PSO/shader hunting list.
	}
	baseList->Release();
}

void DX12HookCommandListCreation(IUnknown *device)
{
	if (!device)
		return;

	ID3D12Device *baseDevice = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&baseDevice)))) {
		void **vtable = *reinterpret_cast<void***>(baseDevice);
		if (vtable) {
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommandList),
				vtable[12], HookedCreateCommandList, "ID3D12Device::CreateCommandList");
		}
		baseDevice->Release();
	}

	ID3D12Device4 *device4 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4)))) {
		void **vtable4 = *reinterpret_cast<void***>(device4);
		if (vtable4) {
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommandList1),
				vtable4[52], HookedCreateCommandList1, "ID3D12Device4::CreateCommandList1");
		}
		device4->Release();
	}
}
