#include "DX12CommandListLifecycle.h"

#include "DX12BindingTracker.h"
#include "DX12CommandListRuntime.h"
#include "DX12CommandListStateCapture.h"
#include "DX12ModRuntime.h"
#include "DX12ShaderHunt.h"

void DX12CommandListLifecycleRegister(
	ID3D12GraphicsCommandList *commandList,
	ID3D12PipelineState *initialState)
{
	if (!commandList)
		return;

	DX12CommandListRuntimeRegister(commandList);
	// Use cached predicates: the tracking cache is populated at Register/Reset
	// time, so subsequent hot-path calls hit a pre-computed answer.
	if (DX12CommandListCaptureShouldTrackBindingsCached(commandList)) {
		DX12BindingRegisterCommandList(commandList);
		DX12BindingResetCommandList(commandList, initialState);
	}
}

void DX12CommandListLifecycleReset(
	ID3D12GraphicsCommandList *commandList,
	ID3D12PipelineState *initialState)
{
	if (!commandList)
		return;

	// Reset makes the command list recordable again. Rebuild every tracked
	// command-list view from the reset PSO before the next recorded command.
	// Use cached predicates populated at Reset time in DX12CommandListRuntimeReset.
	if (DX12CommandListCaptureShouldTrackBindingsCached(commandList))
		DX12BindingResetCommandList(commandList, initialState);
	if (DX12CommandListCaptureShouldTrackPsoStateCached(commandList))
		DX12CommandListRuntimeRememberPipelineState(commandList, initialState);
	if (DX12ModHasActiveTextureOverrides())
		DX12CommandListRuntimeResetIa(commandList);
	if (DX12CommandListCaptureShouldTrackPsoStateCached(commandList) ||
	    DX12CommandListCaptureShouldTrackHuntIaCached(commandList))
		DX12HuntResetCommandList(commandList, initialState);
}

void DX12CommandListLifecycleClearState(
	ID3D12GraphicsCommandList *commandList,
	ID3D12PipelineState *pipelineState)
{
	if (!commandList)
		return;

	if (DX12CommandListCaptureShouldRecordBindingEventsCached(commandList))
		DX12BindingRecordStateEvent(commandList, "clear_state");
	if (DX12CommandListCaptureShouldTrackPsoStateCached(commandList))
		DX12CommandListRuntimeRememberPipelineState(commandList, pipelineState);
	if (DX12CommandListCaptureShouldTrackHuntIaCached(commandList))
		DX12CommandListRuntimeResetIa(commandList);
}
