#pragma once

#include <Windows.h>
#include <d3d12.h>

struct DX12ActiveIaState
{
	bool hasIndexBuffer = false;
	D3D12_INDEX_BUFFER_VIEW indexBuffer = {};
	bool hasVertexBuffer[32] = {};
	D3D12_VERTEX_BUFFER_VIEW vertexBuffers[32] = {};
	D3D12_PRIMITIVE_TOPOLOGY primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
};

// Per-command-list cached tracking decisions.
// The "should track" predicates (e.g. DX12CommandListCaptureShouldTrackBindings)
// check global flags that only change at frame boundaries or when capture starts.
// Evaluating them on every hook call wastes CPU; we snapshot them once at Reset
// and reuse the cached answer on the hot path.  A generation counter lets us
// invalidate all caches cheaply when capture state changes.
struct DX12CommandListTrackingCache
{
	// Generation at which these cached values were computed.
	UINT64 generation = 0;
	bool trackBindings = false;
	bool recordBindingEvents = false;
	bool trackHuntIa = false;
	bool trackPsoState = false;
};

struct DX12CommandListRuntimeState
{
	ID3D12PipelineState *pipelineState = nullptr;
	// computeBindingSerial is bumped on every compute binding change and read on
	// Dispatch.  Use an atomic so the write side does not need an exclusive lock.
	volatile UINT64 computeBindingSerial = 0;
	DX12ActiveIaState ia;
	bool mayHaveIaTextureCandidate = false;
	DX12CommandListTrackingCache trackingCache;

	// Descriptor heaps currently bound to the command list.  Tracked here
	// (instead of in the heavy BindingTracker) so texture-override matching
	// can read them without forcing the full binding-tracking machinery on.
	// Updated unconditionally by HookedSetDescriptorHeaps — two pointer writes
	// are far cheaper than the BindingTracker alternative.
	ID3D12DescriptorHeap *cbvSrvUavHeap = nullptr;
	ID3D12DescriptorHeap *samplerHeap = nullptr;
};

// Global generation bumped whenever capture/hunt/mod state flips, invalidating
// all per-list tracking caches in one go without iterating every list.
UINT64 DX12CommandListRuntimeTrackingGeneration();
void DX12CommandListRuntimeBumpTrackingGeneration();

void DX12CommandListRuntimeRegister(ID3D12GraphicsCommandList *commandList);
void DX12CommandListRuntimeReset(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *initialState);
void DX12CommandListRuntimeRememberPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState);
void DX12CommandListRuntimeResetIa(ID3D12GraphicsCommandList *commandList);
void DX12CommandListRuntimeRememberIndexBuffer(
	ID3D12GraphicsCommandList *commandList, const D3D12_INDEX_BUFFER_VIEW *view);
void DX12CommandListRuntimeRememberVertexBuffers(
	ID3D12GraphicsCommandList *commandList, UINT startSlot, UINT count,
	const D3D12_VERTEX_BUFFER_VIEW *views);
void DX12CommandListRuntimeRememberPrimitiveTopology(
	ID3D12GraphicsCommandList *commandList, D3D12_PRIMITIVE_TOPOLOGY topology);
void DX12CommandListRuntimeSetMayHaveIaTextureCandidate(
	ID3D12GraphicsCommandList *commandList, bool mayHaveCandidate);
bool DX12CommandListRuntimeMayHaveIaTextureCandidate(ID3D12GraphicsCommandList *commandList);
void DX12CommandListRuntimeBumpComputeBindingSerial(ID3D12GraphicsCommandList *commandList);
ID3D12PipelineState *DX12CommandListRuntimeGetPipelineState(
	ID3D12GraphicsCommandList *commandList);
DX12CommandListRuntimeState DX12CommandListRuntimeGetState(
	ID3D12GraphicsCommandList *commandList);

// Lightweight descriptor-heap tracking (used by texture-override matching).
// Updated on every SetDescriptorHeaps call; read on demand via the getter.
void DX12CommandListRuntimeSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList,
	ID3D12DescriptorHeap *cbvSrvUavHeap,
	ID3D12DescriptorHeap *samplerHeap);
bool DX12CommandListRuntimeGetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList,
	ID3D12DescriptorHeap **cbvSrvUavHeap,
	ID3D12DescriptorHeap **samplerHeap);

// --- cached tracking predicates: use these instead of the global checks ---
bool DX12CommandListRuntimeGetTrackBindings(ID3D12GraphicsCommandList *commandList);
bool DX12CommandListRuntimeGetRecordBindingEvents(ID3D12GraphicsCommandList *commandList);
bool DX12CommandListRuntimeGetTrackHuntIa(ID3D12GraphicsCommandList *commandList);
bool DX12CommandListRuntimeGetTrackPsoState(ID3D12GraphicsCommandList *commandList);
