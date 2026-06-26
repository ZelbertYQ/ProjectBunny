#include "DX12CommandListRuntime.h"

#include <unordered_map>

#include "DX12CommandListStateCapture.h"
#include "DX12FrameAnalysis.h"
#include "DX12ModRuntime.h"
#include "DX12ResourceTracker.h"
#include "DX12ShaderDump.h"
#include "DX12ShaderHunt.h"


namespace {

constexpr size_t kRuntimeShardCount = 64;

struct RuntimeShard {
	SRWLOCK lock = SRWLOCK_INIT;
	std::unordered_map<ID3D12GraphicsCommandList*, DX12CommandListRuntimeState> states;
};

RuntimeShard gRuntimeShards[kRuntimeShardCount];

static volatile UINT64 gTrackingGeneration = 1;

inline RuntimeShard &ShardFor(ID3D12GraphicsCommandList *commandList)
{
	uintptr_t key = reinterpret_cast<uintptr_t>(commandList);
	key ^= key >> 7;
	key *= 0x9e3779b97f4a7c15ull;
	return gRuntimeShards[(key >> 17) & (kRuntimeShardCount - 1)];
}

static void RefreshTrackingCache(DX12CommandListTrackingCache &cache)
{
	cache.generation = gTrackingGeneration;
	cache.trackBindings = DX12CommandListCaptureShouldTrackBindings();
	cache.recordBindingEvents = DX12CommandListCaptureShouldRecordBindingEvents();
	cache.trackHuntIa = DX12CommandListCaptureShouldTrackHuntIa();
	cache.trackPsoState = DX12CommandListCaptureShouldTrackPsoState();
}


struct ThreadLocalRuntimeCache {
	ID3D12GraphicsCommandList *commandList = nullptr;
	RuntimeShard *shard = nullptr;

	UINT64 trackingGeneration = 0;
	DX12CommandListTrackingCache tracking;

	DX12CommandListRuntimeState *statePtr = nullptr;
};

static thread_local ThreadLocalRuntimeCache tls;

static void TlsRefresh(RuntimeShard &shard, ID3D12GraphicsCommandList *commandList)
{
	auto it = shard.states.find(commandList);
	if (it == shard.states.end()) {
		tls.commandList = nullptr;
		tls.shard = nullptr;
		tls.statePtr = nullptr;
		tls.trackingGeneration = 0;
		return;
	}

	tls.commandList = commandList;
	tls.shard = &shard;
	tls.statePtr = &it->second;
	tls.trackingGeneration = gTrackingGeneration;

	DX12CommandListTrackingCache &mapCache = it->second.trackingCache;
	if (mapCache.generation != gTrackingGeneration)
		RefreshTrackingCache(mapCache);
	tls.tracking = mapCache;
}

static void GetTrackingCacheFast(
	ID3D12GraphicsCommandList *commandList,
	DX12CommandListTrackingCache &out)
{
	if (tls.commandList == commandList && tls.trackingGeneration == gTrackingGeneration) {
		out = tls.tracking;
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	TlsRefresh(shard, commandList);
	ReleaseSRWLockShared(&shard.lock);

	if (tls.commandList == commandList)
		out = tls.tracking;
}

static DX12CommandListRuntimeState *GetStatePtrFast(
	ID3D12GraphicsCommandList *commandList)
{
	if (tls.commandList == commandList && tls.trackingGeneration == gTrackingGeneration)
		return tls.statePtr;

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	TlsRefresh(shard, commandList);
	ReleaseSRWLockShared(&shard.lock);

	if (tls.commandList == commandList)
		return tls.statePtr;
	return nullptr;
}

static void TlsInvalidateIfMatch(ID3D12GraphicsCommandList *commandList)
{
	if (tls.commandList == commandList)
		tls.commandList = nullptr;
}

}


UINT64 DX12CommandListRuntimeTrackingGeneration()
{
	return gTrackingGeneration;
}

void DX12CommandListRuntimeBumpTrackingGeneration()
{
	InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&gTrackingGeneration));
}

void DX12CommandListRuntimeRegister(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return;

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	shard.states.try_emplace(commandList);
	ReleaseSRWLockExclusive(&shard.lock);
}

void DX12CommandListRuntimeReset(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *initialState)
{
	if (!commandList)
		return;

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	DX12CommandListRuntimeState &state = shard.states[commandList];
	DX12CommandListTrackingCache oldCache = state.trackingCache;
	state = DX12CommandListRuntimeState();
	state.pipelineState = initialState;
	state.trackingCache = oldCache;
	RefreshTrackingCache(state.trackingCache);
	ReleaseSRWLockExclusive(&shard.lock);

	TlsInvalidateIfMatch(commandList);
}

void DX12CommandListRuntimeRememberPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState)
{
	if (!commandList)
		return;

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	shard.states[commandList].pipelineState = pipelineState;
	ReleaseSRWLockExclusive(&shard.lock);
}

void DX12CommandListRuntimeResetIa(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return;

	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState) {
		fastState->ia = DX12ActiveIaState();
		fastState->mayHaveIaTextureCandidate = false;
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	DX12CommandListRuntimeState &state = shard.states[commandList];
	state.ia = DX12ActiveIaState();
	state.mayHaveIaTextureCandidate = false;
	ReleaseSRWLockExclusive(&shard.lock);
}

void DX12CommandListRuntimeRememberIndexBuffer(
	ID3D12GraphicsCommandList *commandList, const D3D12_INDEX_BUFFER_VIEW *view)
{
	if (!commandList)
		return;

	const uint32_t hash = view ?
		DX12HashIaBufferView(
			view->BufferLocation, view->SizeInBytes, 0, static_cast<UINT>(view->Format), 0) :
		0;
	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState) {
		fastState->ia.hasIndexBuffer = view != nullptr;
		fastState->ia.indexBuffer = view ? *view : D3D12_INDEX_BUFFER_VIEW();
		fastState->ia.indexHash = hash;
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	DX12ActiveIaState &state = shard.states[commandList].ia;
	state.hasIndexBuffer = view != nullptr;
	state.indexBuffer = view ? *view : D3D12_INDEX_BUFFER_VIEW();
	state.indexHash = hash;
	ReleaseSRWLockExclusive(&shard.lock);
}

void DX12CommandListRuntimeRememberVertexBuffers(
	ID3D12GraphicsCommandList *commandList, UINT startSlot, UINT count,
	const D3D12_VERTEX_BUFFER_VIEW *views)
{
	if (!commandList)
		return;

	uint32_t hashes[32] = {};
	for (UINT i = 0; i < count && startSlot + i < ARRAYSIZE(hashes); ++i) {
		const UINT slot = startSlot + i;
		const D3D12_VERTEX_BUFFER_VIEW *view = views ? &views[i] : nullptr;
		hashes[slot] = view ?
			DX12HashIaBufferView(
				view->BufferLocation, view->SizeInBytes, view->StrideInBytes, 0, slot) :
			0;
	}

	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState) {
		DX12ActiveIaState &state = fastState->ia;
		for (UINT i = 0; i < count && startSlot + i < ARRAYSIZE(state.vertexBuffers); ++i) {
			const UINT slot = startSlot + i;
			const D3D12_VERTEX_BUFFER_VIEW *view = views ? &views[i] : nullptr;
			state.hasVertexBuffer[slot] = view != nullptr;
			state.vertexBuffers[slot] = view ? *view : D3D12_VERTEX_BUFFER_VIEW();
			state.vertexHashes[slot] = hashes[slot];
		}
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	DX12ActiveIaState &state = shard.states[commandList].ia;
	for (UINT i = 0; i < count && startSlot + i < ARRAYSIZE(state.vertexBuffers); ++i) {
		const UINT slot = startSlot + i;
		const D3D12_VERTEX_BUFFER_VIEW *view = views ? &views[i] : nullptr;
		state.hasVertexBuffer[slot] = view != nullptr;
		state.vertexBuffers[slot] = view ? *view : D3D12_VERTEX_BUFFER_VIEW();
		state.vertexHashes[slot] = hashes[slot];
	}
	ReleaseSRWLockExclusive(&shard.lock);
}

void DX12CommandListRuntimeRememberPrimitiveTopology(
	ID3D12GraphicsCommandList *commandList, D3D12_PRIMITIVE_TOPOLOGY topology)
{
	if (!commandList)
		return;

	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState) {
		fastState->ia.primitiveTopology = topology;
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	shard.states[commandList].ia.primitiveTopology = topology;
	ReleaseSRWLockExclusive(&shard.lock);
}

void DX12CommandListRuntimeSetMayHaveIaTextureCandidate(
	ID3D12GraphicsCommandList *commandList, bool mayHaveCandidate)
{
	if (!commandList)
		return;

	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState) {
		fastState->mayHaveIaTextureCandidate = mayHaveCandidate;
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	shard.states[commandList].mayHaveIaTextureCandidate = mayHaveCandidate;
	ReleaseSRWLockExclusive(&shard.lock);
}

bool DX12CommandListRuntimeMayHaveIaTextureCandidate(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return false;

	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState)
		return fastState->mayHaveIaTextureCandidate;

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	auto it = shard.states.find(commandList);
	const bool result =
		it != shard.states.end() && it->second.mayHaveIaTextureCandidate;
	ReleaseSRWLockShared(&shard.lock);
	return result;
}

void DX12CommandListRuntimeBumpComputeBindingSerial(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return;

	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state) {
		InterlockedIncrement64(
			reinterpret_cast<volatile LONG64*>(&state->computeBindingSerial));
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	auto it = shard.states.find(commandList);
	if (it != shard.states.end())
		InterlockedIncrement64(
			reinterpret_cast<volatile LONG64*>(&it->second.computeBindingSerial));
	ReleaseSRWLockShared(&shard.lock);
}

bool DX12CommandListRuntimeBuildIaHashState(
	const DX12ActiveIaState &ia, DX12IaHashState *state)
{
	if (!state)
		return false;
	*state = DX12IaHashState();

	if (ia.hasIndexBuffer && ia.indexHash) {
		state->hasIndexBuffer = true;
		state->indexHash = ia.indexHash;
		state->indexView = ia.indexBuffer;
	}

	for (UINT slot = 0; slot < ARRAYSIZE(ia.vertexBuffers); ++slot) {
		if (!ia.hasVertexBuffer[slot] || !ia.vertexHashes[slot])
			continue;
		DX12IaBufferHash item;
		item.slot = slot;
		item.hash = ia.vertexHashes[slot];
		item.vertexView = ia.vertexBuffers[slot];
		state->vertexBuffers.push_back(item);
	}

	return state->hasIndexBuffer || !state->vertexBuffers.empty();
}

bool DX12CommandListRuntimeGetIaHashState(
	ID3D12GraphicsCommandList *commandList, DX12IaHashState *state)
{
	if (state)
		*state = DX12IaHashState();
	if (!commandList || !state)
		return false;

	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState)
		return DX12CommandListRuntimeBuildIaHashState(fastState->ia, state);

	RuntimeShard &shard = ShardFor(commandList);
	DX12ActiveIaState ia;
	bool found = false;
	AcquireSRWLockShared(&shard.lock);
	auto it = shard.states.find(commandList);
	if (it != shard.states.end()) {
		ia = it->second.ia;
		found = true;
	}
	ReleaseSRWLockShared(&shard.lock);
	if (!found)
		return false;
	return DX12CommandListRuntimeBuildIaHashState(ia, state);
}

ID3D12PipelineState *DX12CommandListRuntimeGetPipelineState(
	ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return nullptr;

	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state)
		return state->pipelineState;

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	auto it = shard.states.find(commandList);
	ID3D12PipelineState *result =
		it == shard.states.end() ? nullptr : it->second.pipelineState;
	ReleaseSRWLockShared(&shard.lock);
	return result;
}

DX12CommandListRuntimeState DX12CommandListRuntimeGetState(
	ID3D12GraphicsCommandList *commandList)
{
	DX12CommandListRuntimeState result;
	if (!commandList)
		return result;

	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state) {
		result = *state;
		return result;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	auto it = shard.states.find(commandList);
	if (it != shard.states.end())
		result = it->second;
	ReleaseSRWLockShared(&shard.lock);
	return result;
}


bool DX12CommandListRuntimeGetTrackBindings(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return DX12CommandListCaptureShouldTrackBindings();

	DX12CommandListTrackingCache cache;
	GetTrackingCacheFast(commandList, cache);
	return cache.trackBindings;
}

bool DX12CommandListRuntimeGetRecordBindingEvents(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return DX12CommandListCaptureShouldRecordBindingEvents();

	DX12CommandListTrackingCache cache;
	GetTrackingCacheFast(commandList, cache);
	return cache.recordBindingEvents;
}

bool DX12CommandListRuntimeGetTrackHuntIa(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return DX12CommandListCaptureShouldTrackHuntIa();

	DX12CommandListTrackingCache cache;
	GetTrackingCacheFast(commandList, cache);
	return cache.trackHuntIa;
}

bool DX12CommandListRuntimeGetTrackPsoState(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return DX12CommandListCaptureShouldTrackPsoState();

	DX12CommandListTrackingCache cache;
	GetTrackingCacheFast(commandList, cache);
	return cache.trackPsoState;
}


void DX12CommandListRuntimeSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList,
	ID3D12DescriptorHeap *cbvSrvUavHeap,
	ID3D12DescriptorHeap *samplerHeap)
{
	if (!commandList)
		return;

	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state) {
		state->cbvSrvUavHeap = cbvSrvUavHeap;
		state->samplerHeap = samplerHeap;
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	auto it = shard.states.find(commandList);
	if (it != shard.states.end()) {
		it->second.cbvSrvUavHeap = cbvSrvUavHeap;
		it->second.samplerHeap = samplerHeap;
	}
	ReleaseSRWLockExclusive(&shard.lock);
}

bool DX12CommandListRuntimeGetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList,
	ID3D12DescriptorHeap **cbvSrvUavHeap,
	ID3D12DescriptorHeap **samplerHeap)
{
	if (cbvSrvUavHeap)
		*cbvSrvUavHeap = nullptr;
	if (samplerHeap)
		*samplerHeap = nullptr;
	if (!commandList)
		return false;

	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state) {
		if (cbvSrvUavHeap)
			*cbvSrvUavHeap = state->cbvSrvUavHeap;
		if (samplerHeap)
			*samplerHeap = state->samplerHeap;
		return true;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	auto it = shard.states.find(commandList);
	bool found = false;
	if (it != shard.states.end()) {
		if (cbvSrvUavHeap)
			*cbvSrvUavHeap = it->second.cbvSrvUavHeap;
		if (samplerHeap)
			*samplerHeap = it->second.samplerHeap;
		found = true;
	}
	ReleaseSRWLockShared(&shard.lock);
	return found;
}
