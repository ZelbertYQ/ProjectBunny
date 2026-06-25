#include "DX12DispatchHookFlow.h"

#include <vector>

#include "DX12BindingTracker.h"
#include "DX12ModRuntime.h"
#include "DX12Profiling.h"
#include "DX12ShaderDump.h"
#include "DX12State.h"

#if defined(_DEBUG)
#define DX12_DISPATCH_DEBUG_VERBOSE_LOG(stmt) do { stmt; } while (0)
#else
#define DX12_DISPATCH_DEBUG_VERBOSE_LOG(stmt) do {} while (0)
#endif

static void LogPreSkinDispatchProbeReason(
	const char *reason,
	uint64_t computeShaderHash,
	UINT64 computeBindingSerial,
	size_t uavCount)
{
#if defined(_DEBUG)
	// These logs explain why explicit match_cs pre-skinning did not reach the
	// replacement path.
	DX12LogDebugJsonFunc("DX12PreSkinDispatchProbe",
		"\"reason\":\"%s\",\"cs\":\"%016llx\",\"bindingSerial\":%llu,\"uavs\":%zu",
		reason ? reason : "unknown",
		static_cast<unsigned long long>(computeShaderHash),
		static_cast<unsigned long long>(computeBindingSerial),
		uavCount);
#else
	(void)reason;
	(void)computeShaderHash;
	(void)computeBindingSerial;
	(void)uavCount;
#endif
}

struct DX12DispatchPreSkinProbe
{
	std::vector<DX12CurrentComputeUavBinding> uavs;
	std::vector<DX12CurrentComputeUavBinding> srvs;
	std::vector<DX12CurrentComputeUavBinding> cbvs;
	std::vector<DX12CurrentRootConstants> rootConstants;
	bool uavsFound = false;
	bool srvsFound = false;
	bool cbvsFound = false;
	bool rootConstantsFound = false;
	bool applied = false;
	bool shouldLog = false;
	uint64_t computeShaderHash = 0;
	UINT64 computeBindingSerial = 0;
	UINT originalVertexCount = 0;
	UINT overrideVertexCount = 0;
};

static DX12DispatchPreSkinProbe &GetDispatchPreSkinProbeScratch()
{
	static thread_local DX12DispatchPreSkinProbe probe;
	probe.uavs.clear();
	probe.srvs.clear();
	probe.cbvs.clear();
	probe.rootConstants.clear();
	probe.uavsFound = false;
	probe.srvsFound = false;
	probe.cbvsFound = false;
	probe.rootConstantsFound = false;
	probe.applied = false;
	probe.shouldLog = false;
	probe.computeShaderHash = 0;
	probe.computeBindingSerial = 0;
	probe.originalVertexCount = 0;
	probe.overrideVertexCount = 0;
	return probe;
}

static void PreparePreSkinProbe(
	ID3D12GraphicsCommandList *commandList,
	const DX12CommandListRuntimeState &runtimeState,
	DX12DispatchPreSkinProbe *probe)
{
	if (!probe || !DX12ModNeedsPreSkinningUavProbe())
		return;

	DX12PsoShaderInfo shaderInfo = {};
	if (DX12GetPipelineStateShaderInfo(runtimeState.pipelineState, &shaderInfo) &&
	    shaderInfo.hasCS)
		probe->computeShaderHash = shaderInfo.cs;
	if (!probe->computeShaderHash) {
		LogPreSkinDispatchProbeReason("no_pipeline_cs_hash", 0, 0, 0);
		return;
	}
	if (!DX12ModShouldProbePreSkinningForCs(probe->computeShaderHash)) {
		LogPreSkinDispatchProbeReason(
			"cs_not_configured", probe->computeShaderHash, 0, 0);
		return;
	}

	const UINT64 computeBindingSerial =
		DX12BindingGetComputeBindingSerial(commandList);
	if (computeBindingSerial == 0) {
		LogPreSkinDispatchProbeReason(
			"zero_binding_serial", probe->computeShaderHash, 0, 0);
		return;
	}
	probe->computeBindingSerial = computeBindingSerial;
	bool cachedMatch = false;
	if (DX12ModHasCachedPreSkinningUavMatch(
		commandList, probe->computeShaderHash, computeBindingSerial, &cachedMatch)) {
		probe->uavsFound = cachedMatch;
		if (!cachedMatch) {
			LogPreSkinDispatchProbeReason(
				"cached_no_uav_match", probe->computeShaderHash,
				computeBindingSerial, 0);
			return;
		}
	}

#if defined(_DEBUG)
	probe->shouldLog = true;
#endif
	if (!probe->uavsFound)
		probe->uavsFound = DX12BindingGetCurrentComputeUavs(commandList, &probe->uavs);
	if (!probe->uavsFound) {
		LogPreSkinDispatchProbeReason(
			"no_compute_uavs", probe->computeShaderHash,
			computeBindingSerial, probe->uavs.size());
		return;
	}

	// Keep SRV/CBV/root-constant table walking behind the cheap UAV producer
	// filter. Those lookups take tracker locks and can dominate compute-heavy
	// frames when no pre-skin override can match.
	if (probe->uavsFound &&
	    DX12ModPreSkinningUavProducerMayMatch(probe->computeShaderHash, probe->uavs)) {
		probe->srvsFound = DX12BindingGetCurrentComputeSrvs(commandList, &probe->srvs);
		probe->cbvsFound = DX12BindingGetCurrentComputeCbvs(commandList, &probe->cbvs);
		probe->rootConstantsFound =
			DX12BindingGetCurrentComputeRootConstants(commandList, &probe->rootConstants);

		static const std::vector<DX12CurrentComputeUavBinding> kEmptyComputeBindings;
		static const std::vector<DX12CurrentRootConstants> kEmptyRootConstants;
		probe->applied = DX12ModApplyPreSkinningUavReplacement(
			commandList, probe->computeShaderHash, probe->uavs,
			probe->srvsFound ? probe->srvs : kEmptyComputeBindings,
			probe->cbvsFound ? probe->cbvs : kEmptyComputeBindings,
			probe->rootConstantsFound ? probe->rootConstants : kEmptyRootConstants,
			&probe->originalVertexCount, &probe->overrideVertexCount);
	} else {
		LogPreSkinDispatchProbeReason(
			"uav_filter_miss", probe->computeShaderHash,
			computeBindingSerial, probe->uavs.size());
	}

}

static UINT AdjustDispatchGroupsX(
	UINT threadGroupCountX,
	UINT threadGroupCountY,
	UINT threadGroupCountZ,
	const DX12DispatchPreSkinProbe &probe)
{
	if (!probe.applied || !probe.originalVertexCount || !probe.overrideVertexCount ||
	    probe.overrideVertexCount <= probe.originalVertexCount || !threadGroupCountX)
		return threadGroupCountX;

	const UINT threadsPerGroup =
		(probe.originalVertexCount + threadGroupCountX - 1) / threadGroupCountX;
	if (!threadsPerGroup)
		return threadGroupCountX;

	UINT dispatchGroupsX =
		(probe.overrideVertexCount + threadsPerGroup - 1) / threadsPerGroup;
	if (dispatchGroupsX < threadGroupCountX)
		dispatchGroupsX = threadGroupCountX;
	DX12Profiling::RecordPreSkinDispatchResized();
	DX12_DISPATCH_DEBUG_VERBOSE_LOG(
		DX12LogDebugJsonFunc("DX12PreSkinDispatchResize",
			"\"oldGroups\":\"%u,%u,%u\",\"newGroups\":\"%u,%u,%u\",\"oldVertices\":%u,\"newVertices\":%u,\"threadsPerGroup\":%u,\"cs\":\"%016llx\"",
			threadGroupCountX, threadGroupCountY, threadGroupCountZ,
			dispatchGroupsX, threadGroupCountY, threadGroupCountZ,
			probe.originalVertexCount, probe.overrideVertexCount,
			threadsPerGroup,
			static_cast<unsigned long long>(probe.computeShaderHash)));
	return dispatchGroupsX;
}

static void FinishPreSkinProbe(
	ID3D12GraphicsCommandList *commandList,
	UINT threadGroupCountX,
	UINT threadGroupCountY,
	UINT threadGroupCountZ,
	const DX12DispatchPreSkinProbe &probe)
{
	if (probe.applied)
		DX12ModRestorePreSkinningUavReplacement(commandList);
	if (probe.uavsFound)
		DX12ModRecordComputeUavs(commandList, probe.uavs);
	if (probe.computeBindingSerial)
		DX12ModStoreCachedPreSkinningUavMatch(
			commandList, probe.computeShaderHash, probe.computeBindingSerial,
			probe.uavsFound);
	if (!probe.shouldLog)
		return;

	DX12_DISPATCH_DEBUG_VERBOSE_LOG(
		DX12LogDebugJsonFunc("DX12DispatchUavProbe",
			"\"found\":%s,\"uavs\":%zu,\"srvs\":%zu,\"cbvs\":%zu,\"rootConstants\":%zu,\"preSkinApplied\":%s,\"cs\":\"%016llx\",\"groups\":\"%u,%u,%u\"",
			probe.uavsFound ? "true" : "false", probe.uavs.size(),
			probe.srvsFound ? probe.srvs.size() : 0,
			probe.cbvsFound ? probe.cbvs.size() : 0,
			probe.rootConstantsFound ? probe.rootConstants.size() : 0,
			probe.applied ? "true" : "false",
			static_cast<unsigned long long>(probe.computeShaderHash),
			threadGroupCountX, threadGroupCountY, threadGroupCountZ));
}

void DX12DispatchHookFlowExecute(
	ID3D12GraphicsCommandList *commandList,
	UINT threadGroupCountX,
	UINT threadGroupCountY,
	UINT threadGroupCountZ,
	const DX12CommandListRuntimeState &runtimeState,
	DX12DispatchHookFlowDispatch originalDispatch)
{
	if (!originalDispatch)
		return;
	static thread_local bool sInDispatchHookFlow = false;
	if (sInDispatchHookFlow) {
		originalDispatch(commandList, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
		return;
	}
	if (DX12ModHasActiveShaderOverrides() &&
	    DX12ModShouldSkipPipelineState(runtimeState.pipelineState, true))
		return;

	struct DispatchHookFlowGuard {
		bool &active;
		explicit DispatchHookFlowGuard(bool &value) : active(value) { active = true; }
		~DispatchHookFlowGuard() { active = false; }
		DispatchHookFlowGuard(const DispatchHookFlowGuard&) = delete;
		DispatchHookFlowGuard &operator=(const DispatchHookFlowGuard&) = delete;
	} guard(sInDispatchHookFlow);

	DX12DispatchPreSkinProbe &probe = GetDispatchPreSkinProbeScratch();
	PreparePreSkinProbe(commandList, runtimeState, &probe);
	const UINT dispatchGroupsX =
		AdjustDispatchGroupsX(threadGroupCountX, threadGroupCountY, threadGroupCountZ, probe);
	originalDispatch(commandList, dispatchGroupsX, threadGroupCountY, threadGroupCountZ);
	FinishPreSkinProbe(commandList, threadGroupCountX, threadGroupCountY, threadGroupCountZ, probe);
}
