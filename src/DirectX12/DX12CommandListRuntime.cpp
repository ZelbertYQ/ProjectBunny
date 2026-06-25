#include "DX12CommandListRuntime.h"

#include <unordered_map>

#include "DX12CommandListStateCapture.h"
#include "DX12FrameAnalysis.h"
#include "DX12ModRuntime.h"
#include "DX12ShaderDump.h"
#include "DX12ShaderHunt.h"

// Per-command-list runtime state used by the draw/dispatch hot paths.
//
// D3D12 records work into command lists and, per Microsoft's design guidance,
// "each command list is not free-threaded; however, multiple command lists can
// be recorded concurrently"
// (https://learn.microsoft.com/windows/win32/direct3d12/design-philosophy-of-command-queues-and-command-lists).
//
// Performance-critical optimizations (cumulative):
//  1. computeBindingSerial uses InterlockedIncrement64 — no exclusive lock.
//  2. Tracking predicates cached per-list, invalidated via global generation.
//  3. **TLS hot-path cache** — A D3D12 command list is recorded by exactly one
//     thread at a time, and consecutive API calls almost always target the same
//     list.  We cache the tracking decisions and runtime-state pointer in
//     thread-local storage so the steady-state hot path is: one pointer compare
//     + one generation compare → direct struct access.  No hash, no lock, no
//     unordered_map::find on every call.

// ---------------------------------------------------------------------------
// Sharded global map (fallback / cross-thread access)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Thread-local hot-path cache
// ---------------------------------------------------------------------------
// Because a command list is recorded by a single thread at a time and the vast
// majority of consecutive API calls target the same list, caching the last-used
// command list + its derived data in TLS turns the steady-state hot path into:
//
//   if (tls.list == commandList && tls.gen == gTrackingGeneration)
//       return tls.cachedAnswer;
//
// No hash, no lock, no unordered_map lookup.  The slow path (first call after
// a list switch or generation bump) does one map lookup and caches the result.

struct ThreadLocalRuntimeCache {
	ID3D12GraphicsCommandList *commandList = nullptr;
	RuntimeShard *shard = nullptr;

	// Cached tracking decisions (copy, not pointer — safe against map rehash).
	UINT64 trackingGeneration = 0;
	DX12CommandListTrackingCache tracking;

	// Cached pointer into the shard's state map for fast pipelineState reads.
	// Only valid while trackingGeneration == gTrackingGeneration AND
	// commandList matches.  Must be re-fetched after generation bumps because
	// the map entry pointer can be invalidated by rehash on insert.
	DX12CommandListRuntimeState *statePtr = nullptr;
};

static thread_local ThreadLocalRuntimeCache tls;

// Populate TLS cache from the global map.  Caller must hold shard.lock shared.
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

	// Refresh the tracking cache from the map entry, then copy to TLS.
	DX12CommandListTrackingCache &mapCache = it->second.trackingCache;
	if (mapCache.generation != gTrackingGeneration)
		RefreshTrackingCache(mapCache);
	tls.tracking = mapCache;
}

// Fast-path tracking read.  Returns the tracking decisions for commandList
// without any lock or map lookup in the common case (TLS hit).  The ShardFor
// hash is only computed on TLS miss, saving another ~10 CPU instructions per
// hot-path call.
static void GetTrackingCacheFast(
	ID3D12GraphicsCommandList *commandList,
	DX12CommandListTrackingCache &out)
{
	// Steady-state hot path: same list, same generation → TLS hit.
	// No hash, no lock, no map — just two compares and a struct copy.
	if (tls.commandList == commandList && tls.trackingGeneration == gTrackingGeneration) {
		out = tls.tracking;
		return;
	}

	// Slow path: compute shard, take lock, populate TLS from map.
	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	TlsRefresh(shard, commandList);
	ReleaseSRWLockShared(&shard.lock);

	if (tls.commandList == commandList)
		out = tls.tracking;
}

// Fast-path runtime state pointer lookup.  Same TLS pattern.
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

// Invalidate TLS for a command list after its state is modified.
static void TlsInvalidateIfMatch(ID3D12GraphicsCommandList *commandList)
{
	if (tls.commandList == commandList)
		tls.commandList = nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

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

	// TLS is now stale for this list — next access will repopulate from map.
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

	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState) {
		fastState->ia.hasIndexBuffer = view != nullptr;
		fastState->ia.indexBuffer = view ? *view : D3D12_INDEX_BUFFER_VIEW();
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	DX12ActiveIaState &state = shard.states[commandList].ia;
	state.hasIndexBuffer = view != nullptr;
	state.indexBuffer = view ? *view : D3D12_INDEX_BUFFER_VIEW();
	ReleaseSRWLockExclusive(&shard.lock);
}

void DX12CommandListRuntimeRememberVertexBuffers(
	ID3D12GraphicsCommandList *commandList, UINT startSlot, UINT count,
	const D3D12_VERTEX_BUFFER_VIEW *views)
{
	if (!commandList)
		return;

	DX12CommandListRuntimeState *fastState = GetStatePtrFast(commandList);
	if (fastState) {
		DX12ActiveIaState &state = fastState->ia;
		for (UINT i = 0; i < count && startSlot + i < ARRAYSIZE(state.vertexBuffers); ++i) {
			state.hasVertexBuffer[startSlot + i] = views != nullptr;
			state.vertexBuffers[startSlot + i] = views ? views[i] : D3D12_VERTEX_BUFFER_VIEW();
		}
		return;
	}

	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockExclusive(&shard.lock);
	DX12ActiveIaState &state = shard.states[commandList].ia;
	for (UINT i = 0; i < count && startSlot + i < ARRAYSIZE(state.vertexBuffers); ++i) {
		state.hasVertexBuffer[startSlot + i] = views != nullptr;
		state.vertexBuffers[startSlot + i] = views ? views[i] : D3D12_VERTEX_BUFFER_VIEW();
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

	// IA candidate state is updated by IASet hooks and read by Draw hooks on
	// the same recording thread. Use the TLS state pointer so burst frames do
	// not serialize thousands of IA writes through the shard lock.
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

	// TLS fast path: direct access to serial field without map lookup.
	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state) {
		InterlockedIncrement64(
			reinterpret_cast<volatile LONG64*>(&state->computeBindingSerial));
		return;
	}

	// Slow path (first access or TLS miss).
	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	auto it = shard.states.find(commandList);
	if (it != shard.states.end())
		InterlockedIncrement64(
			reinterpret_cast<volatile LONG64*>(&it->second.computeBindingSerial));
	ReleaseSRWLockShared(&shard.lock);
}

ID3D12PipelineState *DX12CommandListRuntimeGetPipelineState(
	ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return nullptr;

	// TLS fast path.
	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state)
		return state->pipelineState;

	// Slow path.
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

	// TLS fast path.
	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state) {
		result = *state;
		return result;
	}

	// Slow path.
	RuntimeShard &shard = ShardFor(commandList);
	AcquireSRWLockShared(&shard.lock);
	auto it = shard.states.find(commandList);
	if (it != shard.states.end())
		result = it->second;
	ReleaseSRWLockShared(&shard.lock);
	return result;
}

// --- cached tracking predicate accessors ---
// Steady-state hot path: one pointer compare + one generation compare → bool.
// No hash, no lock, no map lookup.

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

// --- lightweight descriptor-heap tracking ---
// Texture-override matching needs to know which descriptor heaps are bound,
// but it does NOT need the full BindingTracker (root-table tracking, event
// recording, etc.).  We store the two heap pointers directly in the runtime
// state — two unconditional pointer writes on SetDescriptorHeaps, two pointer
// reads on query.  No locks, no hashing beyond the standard TLS fast path.

void DX12CommandListRuntimeSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList,
	ID3D12DescriptorHeap *cbvSrvUavHeap,
	ID3D12DescriptorHeap *samplerHeap)
{
	if (!commandList)
		return;

	// TLS fast path: write directly into the cached state pointer.
	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state) {
		state->cbvSrvUavHeap = cbvSrvUavHeap;
		state->samplerHeap = samplerHeap;
		return;
	}

	// Slow path.
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

	// TLS fast path.
	DX12CommandListRuntimeState *state = GetStatePtrFast(commandList);
	if (state) {
		if (cbvSrvUavHeap)
			*cbvSrvUavHeap = state->cbvSrvUavHeap;
		if (samplerHeap)
			*samplerHeap = state->samplerHeap;
		return true;
	}

	// Slow path.
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
