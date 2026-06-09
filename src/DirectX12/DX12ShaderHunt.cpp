#include "DX12ShaderHunt.h"

#include <Windows.h>

#include <set>
#include <unordered_map>

#include "DX12ShaderDump.h"
#include "DX12State.h"

enum class HuntStage
{
	VS,
	PS,
	CS
};

struct StageSelection
{
	std::set<UINT64> visited;
	size_t selected = 0;
};

static SRWLOCK gHuntLock = SRWLOCK_INIT;
static bool gHuntingEnabled = true;
static std::unordered_map<ID3D12GraphicsCommandList*, ID3D12PipelineState*> gCommandListPso;
static StageSelection gVS;
static StageSelection gPS;
static StageSelection gCS;
static UINT64 gPipelineStateBinds = 0;
static UINT64 gDrawCalls = 0;
static UINT64 gDispatchCalls = 0;
static UINT64 gIndirectCalls = 0;

static UINT64 SelectedHashLocked(const StageSelection &stage)
{
	if (stage.visited.empty())
		return 0;

	size_t index = stage.selected % stage.visited.size();
	auto it = stage.visited.begin();
	std::advance(it, index);
	return *it;
}

static void AddVisitedLocked(StageSelection &stage, UINT64 hash)
{
	if (!hash)
		return;
	stage.visited.insert(hash);
	if (!stage.visited.empty())
		stage.selected %= stage.visited.size();
}

static void AdvanceStage(StageSelection &stage, int delta)
{
	AcquireSRWLockExclusive(&gHuntLock);
	if (!stage.visited.empty()) {
		const size_t size = stage.visited.size();
		if (delta < 0)
			stage.selected = (stage.selected + size - 1) % size;
		else
			stage.selected = (stage.selected + 1) % size;
	}
	ReleaseSRWLockExclusive(&gHuntLock);
	DX12HuntRefreshOverlay();
}

static void RecordPipelineStateUseLocked(ID3D12PipelineState *pipelineState, bool draw, bool dispatch)
{
	DX12PsoShaderInfo info = {};
	if (!pipelineState || !DX12GetPipelineStateShaderInfo(pipelineState, &info))
		return;

	if (draw) {
		AddVisitedLocked(gVS, info.hasVS ? info.vs : 0);
		AddVisitedLocked(gPS, info.hasPS ? info.ps : 0);
	}
	if (dispatch)
		AddVisitedLocked(gCS, info.hasCS ? info.cs : 0);
}

static ID3D12PipelineState *GetCurrentPipelineStateLocked(ID3D12GraphicsCommandList *commandList)
{
	auto it = gCommandListPso.find(commandList);
	if (it == gCommandListPso.end())
		return nullptr;
	return it->second;
}

void DX12HuntSetPipelineState(ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gHuntLock);
	gCommandListPso[commandList] = pipelineState;
	gPipelineStateBinds++;
	RecordPipelineStateUseLocked(pipelineState, true, true);
	ReleaseSRWLockExclusive(&gHuntLock);
}

void DX12HuntRecordDraw(ID3D12GraphicsCommandList *commandList)
{
	if (!gHuntingEnabled || !commandList)
		return;

	AcquireSRWLockExclusive(&gHuntLock);
	gDrawCalls++;
	RecordPipelineStateUseLocked(GetCurrentPipelineStateLocked(commandList), true, false);
	ReleaseSRWLockExclusive(&gHuntLock);
}

void DX12HuntRecordDispatch(ID3D12GraphicsCommandList *commandList)
{
	if (!gHuntingEnabled || !commandList)
		return;

	AcquireSRWLockExclusive(&gHuntLock);
	gDispatchCalls++;
	RecordPipelineStateUseLocked(GetCurrentPipelineStateLocked(commandList), false, true);
	ReleaseSRWLockExclusive(&gHuntLock);
}

void DX12HuntRecordExecuteIndirect(ID3D12GraphicsCommandList *commandList)
{
	if (!gHuntingEnabled || !commandList)
		return;

	AcquireSRWLockExclusive(&gHuntLock);
	gIndirectCalls++;
	RecordPipelineStateUseLocked(GetCurrentPipelineStateLocked(commandList), true, true);
	ReleaseSRWLockExclusive(&gHuntLock);
}

void DX12HuntToggle()
{
	AcquireSRWLockExclusive(&gHuntLock);
	gHuntingEnabled = !gHuntingEnabled;
	bool enabled = gHuntingEnabled;
	ReleaseSRWLockExclusive(&gHuntLock);

	DX12Log("DX12 hunting toggled %s\n", enabled ? "on" : "off");
	DX12HuntRefreshOverlay();
}

void DX12HuntPreviousVS() { AdvanceStage(gVS, -1); }
void DX12HuntNextVS() { AdvanceStage(gVS, 1); }
void DX12HuntPreviousPS() { AdvanceStage(gPS, -1); }
void DX12HuntNextPS() { AdvanceStage(gPS, 1); }
void DX12HuntPreviousCS() { AdvanceStage(gCS, -1); }
void DX12HuntNextCS() { AdvanceStage(gCS, 1); }

void DX12HuntRefreshOverlay()
{
	wchar_t status[512];

	AcquireSRWLockShared(&gHuntLock);
	const UINT64 vs = SelectedHashLocked(gVS);
	const UINT64 ps = SelectedHashLocked(gPS);
	const UINT64 cs = SelectedHashLocked(gCS);
	swprintf_s(status,
		L"DX12 Hunting: %ls\n"
		L"VS %zu/%zu %016llx   PS %zu/%zu %016llx   CS %zu/%zu %016llx\n"
		L"PSO binds %llu   Draw %llu Dispatch %llu Indirect %llu   F7 toggle  4/5 VS  6/7 PS  8/9 CS  F8 dump",
		gHuntingEnabled ? L"on" : L"off",
		gVS.visited.empty() ? 0 : gVS.selected + 1, gVS.visited.size(),
		static_cast<unsigned long long>(vs),
		gPS.visited.empty() ? 0 : gPS.selected + 1, gPS.visited.size(),
		static_cast<unsigned long long>(ps),
		gCS.visited.empty() ? 0 : gCS.selected + 1, gCS.visited.size(),
		static_cast<unsigned long long>(cs),
		static_cast<unsigned long long>(gPipelineStateBinds),
		static_cast<unsigned long long>(gDrawCalls),
		static_cast<unsigned long long>(gDispatchCalls),
		static_cast<unsigned long long>(gIndirectCalls));
	ReleaseSRWLockShared(&gHuntLock);

	DX12SetOverlayStatus(status);
}
