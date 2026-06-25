#include "DX12IaReplacementExecutor.h"

#include "DX12Profiling.h"
#include "DX12ShaderHunt.h"

static thread_local int tIaReplacementDrawDepth = 0;

struct DX12IaReplacementDrawScope
{
	DX12IaReplacementDrawScope()
	{
		++tIaReplacementDrawDepth;
	}

	~DX12IaReplacementDrawScope()
	{
		--tIaReplacementDrawDepth;
	}
};

bool DX12IaReplacementIsExecutingInternalDraw()
{
	return tIaReplacementDrawDepth > 0;
}

static bool PrepareIaForMod(
	ID3D12GraphicsCommandList *commandList,
	DX12IaHashState *iaState,
	const DX12IaDrawInvocation &draw,
	DX12ModIaReplacement *replacement)
{
	if (!DX12ModHasActiveTextureOverrides())
		return false;
	if (!iaState || !DX12HuntGetIaHashState(commandList, iaState))
		return false;

	const uint32_t vertexCount = draw.indexed ? 0 : draw.vertexCount;
	const uint32_t indexCount = draw.indexed ? draw.indexCount : 0;
	const uint32_t firstVertex = draw.indexed ? 0 : draw.firstVertex;
	const uint32_t firstIndex = draw.indexed ? draw.firstIndex : 0;
	return DX12ModPrepareIaReplacement(
		commandList, *iaState, vertexCount, indexCount, draw.instanceCount,
		firstVertex, firstIndex, draw.firstInstance, replacement);
}

static void ApplyIaReplacement(
	ID3D12GraphicsCommandList *commandList,
	const DX12ModIaReplacement &replacement,
	const DX12IaReplacementExecutorCallbacks &callbacks)
{
	if (replacement.hasIndexBuffer && callbacks.setIndexBuffer) {
		DX12CommandListRuntimeRememberIndexBuffer(commandList, &replacement.indexBuffer);
		callbacks.setIndexBuffer(commandList, &replacement.indexBuffer);
	}

	if (!replacement.vertexBuffers.empty() && callbacks.setVertexBuffers) {
		DX12CommandListRuntimeRememberVertexBuffers(commandList, replacement.vertexBufferStartSlot,
			static_cast<UINT>(replacement.vertexBuffers.size()),
			replacement.vertexBuffers.data());
		callbacks.setVertexBuffers(commandList, replacement.vertexBufferStartSlot,
			static_cast<UINT>(replacement.vertexBuffers.size()),
			replacement.vertexBuffers.data());
	}
}

static void RestoreIaReplacement(
	ID3D12GraphicsCommandList *commandList,
	const DX12ModIaReplacement &replacement,
	const DX12ActiveIaState &originalState,
	const DX12IaReplacementExecutorCallbacks &callbacks)
{
	if (replacement.hasIndexBuffer && callbacks.setIndexBuffer) {
		DX12CommandListRuntimeRememberIndexBuffer(commandList,
			originalState.hasIndexBuffer ? &originalState.indexBuffer : nullptr);
		callbacks.setIndexBuffer(commandList,
			originalState.hasIndexBuffer ? &originalState.indexBuffer : nullptr);
	}

	if (!replacement.vertexBuffers.empty() && callbacks.setVertexBuffers) {
		D3D12_VERTEX_BUFFER_VIEW restore[32] = {};
		const UINT start = replacement.vertexBufferStartSlot;
		const UINT count = static_cast<UINT>(replacement.vertexBuffers.size());
		for (UINT i = 0; i < count && start + i < ARRAYSIZE(originalState.vertexBuffers); ++i)
			restore[i] = originalState.hasVertexBuffer[start + i] ?
				originalState.vertexBuffers[start + i] : D3D12_VERTEX_BUFFER_VIEW();
		DX12CommandListRuntimeRememberVertexBuffers(commandList, start, count, restore);
		callbacks.setVertexBuffers(commandList, start, count, restore);
	}
}

static void ExecuteOriginalDraw(
	ID3D12GraphicsCommandList *commandList,
	const DX12IaDrawInvocation &draw,
	const DX12IaReplacementExecutorCallbacks &callbacks)
{
	if (draw.indexed) {
		if (callbacks.drawIndexedInstanced) {
			callbacks.drawIndexedInstanced(commandList, draw.indexCount, draw.instanceCount,
				draw.firstIndex, draw.baseVertex, draw.firstInstance);
		}
		return;
	}

	if (callbacks.drawInstanced) {
		callbacks.drawInstanced(commandList, draw.vertexCount, draw.instanceCount,
			draw.firstVertex, draw.firstInstance);
	}
}

static void ExecuteReplacementDraws(
	ID3D12GraphicsCommandList *commandList,
	const DX12ModIaReplacement &replacement,
	const DX12IaDrawInvocation &originalDraw,
	const DX12IaReplacementExecutorCallbacks &callbacks)
{
	for (const DX12ModIaReplacement::DispatchCall &dispatch : replacement.dispatches) {
		if (!dispatch.groupsX || !dispatch.groupsY || !dispatch.groupsZ || !callbacks.dispatch)
			continue;
		if (callbacks.recordReplacementDispatch)
			callbacks.recordReplacementDispatch();
		callbacks.dispatch(commandList, dispatch.groupsX, dispatch.groupsY, dispatch.groupsZ);
	}

	for (const DX12ModIaReplacement::DrawCall &draw : replacement.draws) {
		UINT count = draw.count;
		UINT start = draw.start;
		INT baseVertex = draw.baseVertex;

		// draw = from_caller: replay the original draw with the replacement
		// resources bound.  This matches DX11's FROM_CALLER behaviour — the mod
		// says "use whatever the game was going to draw" without hardcoding
		// vertex/index counts that may drift across game patches or LODs.
		if (draw.fromCaller) {
			if (draw.indexed) {
				count = originalDraw.indexCount;
				start = originalDraw.firstIndex;
				baseVertex = originalDraw.baseVertex;
			} else {
				count = originalDraw.vertexCount;
				start = originalDraw.firstVertex;
			}
			if (!count)
				continue;
		} else {
			if (!draw.count)
				continue;
		}

		DX12IaReplacementDrawScope replacementDrawScope;
		if (draw.indexed) {
			if (callbacks.drawIndexedInstanced) {
				if (callbacks.recordReplacementDraw)
					callbacks.recordReplacementDraw(true);
				callbacks.drawIndexedInstanced(
					commandList, count, 1, start, baseVertex, 0);
			}
		} else if (callbacks.drawInstanced) {
			if (callbacks.recordReplacementDraw)
				callbacks.recordReplacementDraw(false);
			callbacks.drawInstanced(commandList, count, 1, start, 0);
		}
	}
}

bool DX12IaReplacementExecutePresent(
	ID3D12GraphicsCommandList *commandList,
	const DX12CommandListRuntimeState &runtimeState,
	const DX12IaReplacementExecutorCallbacks &callbacks)
{
	DX12ModIaReplacement replacement;
	if (!DX12ModPreparePresentReplacement(commandList, &replacement))
		return false;

	const DX12ActiveIaState originalIa = runtimeState.ia;
	ApplyIaReplacement(commandList, replacement, callbacks);
	// Present replacement has no "original draw" to borrow from; from_caller
	// draws in this context receive zero counts and are skipped (expected).
	DX12IaDrawInvocation noDraw = {};
	ExecuteReplacementDraws(commandList, replacement, noDraw, callbacks);
	RestoreIaReplacement(commandList, replacement, originalIa, callbacks);
	return true;
}

bool DX12IaReplacementApplyAndExecute(
	ID3D12GraphicsCommandList *commandList,
	const DX12IaDrawInvocation &draw,
	const DX12IaHashState &iaState,
	DX12ModIaReplacement *replacement,
	bool fromShaderOverride,
	const DX12CommandListRuntimeState &runtimeState,
	const DX12IaReplacementExecutorCallbacks &callbacks)
{
	if (!commandList || !replacement)
		return false;
	if (replacement->skip && replacement->draws.empty() && replacement->dispatches.empty()) {
		if (fromShaderOverride)
			return true;
		return false;
	}

	if (!fromShaderOverride && callbacks.shouldSuppressAutoReplacement &&
	    callbacks.shouldSuppressAutoReplacement(
		    commandList, iaState,
		    draw.vertexCount, draw.indexCount, draw.instanceCount,
		    draw.firstVertex, draw.firstIndex, draw.baseVertex,
		    draw.firstInstance, *replacement)) {
		// D3D12 draw and dispatch calls append GPU work to the command list.
		// When the same IA replacement appears repeatedly in one frame, replaying
		// all replacement/post commands again can flood the GPU queue at startup.
		return true;
	}

	const DX12ActiveIaState originalIa = runtimeState.ia;
	ApplyIaReplacement(commandList, *replacement, callbacks);

	// DX12 records replacement work into the currently intercepted command list.
	// Keeping this execution rule in one module gives later ResourceTarget and
	// barrier handling the same locality that DX11 gets from CommandListState.
	if (!replacement->skip)
		ExecuteOriginalDraw(commandList, draw, callbacks);

	ExecuteReplacementDraws(commandList, *replacement, draw, callbacks);
	RestoreIaReplacement(commandList, *replacement, originalIa, callbacks);
	DX12Profiling::RecordIaApplied();
	if (fromShaderOverride) {
		DX12ModRunPostShaderOverrideReplacement(
			commandList, runtimeState.pipelineState, iaState,
			draw.vertexCount, draw.indexCount, draw.instanceCount,
			draw.firstVertex, draw.firstIndex, replacement);
	} else {
		DX12ModRunPostIaReplacement(
			commandList, iaState, draw.vertexCount, draw.indexCount, draw.instanceCount,
			draw.firstVertex, draw.firstIndex, draw.firstInstance, replacement);
	}
	return true;
}

bool DX12IaReplacementHandleDrawOverrides(
	ID3D12GraphicsCommandList *commandList,
	const DX12IaDrawInvocation &draw,
	const DX12CommandListRuntimeState &runtimeState,
	const DX12IaReplacementExecutorCallbacks &callbacks)
{
	if (!DX12ModHasAnyActiveOverrides())
		return false;

	DX12Profiling::RecordIaDrawOverrideCheck();
	DX12ModIaReplacement iaReplacement;
	DX12IaHashState iaState;
	const uint32_t vertexCount = draw.indexed ? 0 : draw.vertexCount;
	const uint32_t indexCount = draw.indexed ? draw.indexCount : 0;
	const uint32_t firstVertex = draw.indexed ? 0 : draw.firstVertex;
	const uint32_t firstIndex = draw.indexed ? draw.firstIndex : 0;

	if (DX12HuntGetIaHashState(commandList, &iaState)) {
		DX12Profiling::RecordIaHashStateResult(true);
		if (DX12ModHasActiveShaderOverrides() &&
		    DX12ModPrepareShaderOverrideReplacement(
			    commandList, runtimeState.pipelineState, iaState,
			    vertexCount, indexCount, draw.instanceCount,
			    firstVertex, firstIndex, &iaReplacement)) {
			if (DX12IaReplacementApplyAndExecute(
				commandList, draw, iaState, &iaReplacement, true,
				runtimeState, callbacks))
				return true;
		}
		const bool mayHaveTextureMatch =
			DX12ModHasActiveTextureOverrides() &&
			DX12ModIaMayHaveTextureOverrideMatch(iaState, draw.indexed);
		DX12Profiling::RecordIaTextureMayMatchResult(mayHaveTextureMatch);
		if (!mayHaveTextureMatch)
			return false;
		DX12Profiling::RecordIaPrepareCall();
		if (DX12ModPrepareIaReplacement(
			commandList, iaState, vertexCount, indexCount, draw.instanceCount,
			firstVertex, firstIndex, draw.firstInstance, &iaReplacement)) {
			if (DX12IaReplacementApplyAndExecute(
				commandList, draw, iaState, &iaReplacement, false,
				runtimeState, callbacks))
				return true;
		}
		return false;
	}

	DX12Profiling::RecordIaHashStateResult(false);
	if (DX12ModShouldSkipPipelineState(runtimeState.pipelineState, false))
		return true;
	if (!PrepareIaForMod(commandList, &iaState, draw, &iaReplacement))
		return false;
	if (iaReplacement.skip && iaReplacement.draws.empty() &&
	    iaReplacement.dispatches.empty())
		return false;
	return DX12IaReplacementApplyAndExecute(
		commandList, draw, iaState, &iaReplacement, false, runtimeState, callbacks);
}
