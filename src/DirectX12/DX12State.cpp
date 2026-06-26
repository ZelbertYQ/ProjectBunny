#include "DX12State.h"

#include <Shlwapi.h>
#include <atomic>
#include <deque>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "DX12FrameAnalysis.h"
#include "DX12Json.h"
#include "DX12ModRuntime.h"
#include "DX12ShaderDump.h"
#include "DX12ShaderHunt.h"
#include "Nektra/NktHookLib.h"

static HINSTANCE gModule = nullptr;
static FILE *gLog = nullptr;
static HANDLE gLogThread = nullptr;
static HANDLE gLogEvent = nullptr;
static SRWLOCK gLogQueueLock = SRWLOCK_INIT;
static std::deque<std::string> gLogQueue;
static volatile LONG gLogDroppedLines = 0;
static volatile LONG gLogStopRequested = 0;
static volatile LONG gPresentCount = 0;
static HWND gOverlayWindow = nullptr;
static wchar_t gOverlayStatus[512] = L"3DMigoto DX12 hook alive";
static COLORREF gOverlayStatusColor = RGB(0, 255, 0);
static DWORD gOverlayStatusExpireTick = 0;
static ID3D12CommandQueue *gCommandQueue = nullptr;
static SRWLOCK gStateLock = SRWLOCK_INIT;
static std::atomic<bool> gDiagnosticsLogging{ false };
#if !defined(_DEBUG)
static std::atomic<bool> gReleaseStartupLogging{ true };
#endif
static CNktHookLib gHookMgr;
static std::unordered_set<void*> gHookedFunctions;
static std::unordered_map<void*, void*> gOriginalFunctions;

volatile LONG gDX12HotPathSkipAll = 1;
volatile LONG gDX12HotPathSkipBindings = 1;
volatile LONG gDX12HotPathTrackResourceMetadata = 0;
static thread_local UINT tDX12InternalReplayDepth = 0;

#if defined(_DEBUG)
struct DX12HookCallLogBudget
{
	LONG present = -1;
	LONG count = 0;
};

static constexpr LONG kHookCallLogBudgetPerApiPerPresent = 16;
static SRWLOCK gHookCallLogBudgetLock = SRWLOCK_INIT;
static std::unordered_map<std::string, DX12HookCallLogBudget> gHookCallLogBudgets;

static bool DX12IsNoisyHookCallApi(const char *api)
{
	static const char *const noisyApis[] = {
		"ID3D12Device::CopyDescriptors",
		"ID3D12Device::CopyDescriptorsSimple",
		"ID3D12Device::CreateConstantBufferView",
		"ID3D12GraphicsCommandList::Dispatch",
		"ID3D12GraphicsCommandList::DrawInstanced",
		"ID3D12GraphicsCommandList::DrawIndexedInstanced",
		"ID3D12GraphicsCommandList::SetPipelineState",
		"ID3D12GraphicsCommandList::SetComputeRootDescriptorTable",
		"ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable",
		"ID3D12GraphicsCommandList::SetComputeRoot32BitConstant",
		"ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant",
		"ID3D12GraphicsCommandList::SetComputeRoot32BitConstants",
		"ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants",
		"ID3D12GraphicsCommandList::SetComputeRootConstantBufferView",
		"ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView",
		"ID3D12GraphicsCommandList::SetComputeRootShaderResourceView",
		"ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView",
		"ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView",
		"ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView",
		"ID3D12GraphicsCommandList::IASetIndexBuffer",
		"ID3D12GraphicsCommandList::IASetVertexBuffers",
		"ID3D12GraphicsCommandList::ResourceBarrier",
		"ID3D12GraphicsCommandList::Reset",
		"ID3D12CommandQueue::ExecuteCommandLists"
	};
	for (const char *noisyApi : noisyApis) {
		if (!strcmp(api, noisyApi))
			return true;
	}
	return false;
}
#endif

static constexpr size_t kMaxQueuedLogLines = 8192;

void DX12EnterInternalReplay()
{
	++tDX12InternalReplayDepth;
}

void DX12LeaveInternalReplay()
{
	if (tDX12InternalReplayDepth)
		--tDX12InternalReplayDepth;
}

bool DX12IsInternalReplay()
{
	return tDX12InternalReplayDepth != 0;
}

void DX12HotPathUpdate()
{
	const bool needsHeavyTracking =
		DX12FrameAnalysisIsActive() ||
		DX12FrameAnalysisIsCapturing() ||
		DX12FrameAnalysisIsCaptureRequested() ||
		DX12ShaderDumpIsCapturingFrame() ||
		DX12ShaderDumpIsCaptureRequested() ||
		DX12ShaderDumpIsBusy();

	const bool needsRecordWork =
		DX12HuntIsEnabled() ||
		DX12ModHasActiveShaderOverrides() ||
		DX12ModHasActiveTextureOverrides() ||
		DX12ModNeedsPresentReplacement() ||
		DX12ModHasAnyActiveOverrides() ||
		DX12ModNeedsPreSkinningUavProbe();
	const bool needsBindingState =
		needsHeavyTracking ||
		DX12ModNeedsPreSkinningUavProbe();

	InterlockedExchange(&gDX12HotPathSkipAll,
		(needsHeavyTracking || needsRecordWork) ? 0 : 1);

	InterlockedExchange(&gDX12HotPathSkipBindings,
		needsBindingState ? 0 : 1);

	InterlockedExchange(&gDX12HotPathTrackResourceMetadata,
		(needsHeavyTracking || DX12ModNeedsPreSkinningUavProbe()) ? 1 : 0);
}

bool DX12ShouldLogHookCall(const char *api)
{
#if defined(_DEBUG)
	if (!DX12DiagnosticsLoggingEnabled() || !api || !api[0] || DX12IsInternalReplay())
		return false;
	if (DX12IsNoisyHookCallApi(api))
		return false;

	const LONG present = DX12GetPresentCount();
	bool shouldLog = false;
	AcquireSRWLockExclusive(&gHookCallLogBudgetLock);
	DX12HookCallLogBudget &budget = gHookCallLogBudgets[api];
	if (budget.present != present) {
		budget.present = present;
		budget.count = 0;
	}
	if (budget.count < kHookCallLogBudgetPerApiPerPresent) {
		++budget.count;
		shouldLog = true;
	}
	if (gHookCallLogBudgets.size() > 256)
		gHookCallLogBudgets.clear();
	ReleaseSRWLockExclusive(&gHookCallLogBudgetLock);
	return shouldLog;
#else
	(void)api;
	return false;
#endif
}

void DX12SetModule(HINSTANCE module)
{
	gModule = module;
}

HINSTANCE DX12GetModule()
{
	return gModule;
}

static bool ReadDiagnosticsLoggingEnabled()
{
	wchar_t value[32] = {};
	const DWORD chars = GetEnvironmentVariableW(L"MIGOTO_DX12_DIAGNOSTIC_LOGS", value, ARRAYSIZE(value));
	if (chars == 0 || chars >= ARRAYSIZE(value))
		return false;
	return !_wcsicmp(value, L"1") ||
		!_wcsicmp(value, L"true") ||
		!_wcsicmp(value, L"on") ||
		!_wcsicmp(value, L"yes");
}

bool DX12DiagnosticsLoggingEnabled()
{
	return gDiagnosticsLogging.load(std::memory_order_relaxed);
}

static DWORD WINAPI DX12LogThreadProc(void*)
{
	for (;;) {
		if (gLogEvent)
			WaitForSingleObject(gLogEvent, 100);
		if (gLogEvent)
			ResetEvent(gLogEvent);

		std::vector<std::string> pending;
		AcquireSRWLockExclusive(&gLogQueueLock);
		while (!gLogQueue.empty()) {
			pending.push_back(std::move(gLogQueue.front()));
			gLogQueue.pop_front();
		}
		const bool stopping = InterlockedCompareExchange(&gLogStopRequested, 0, 0) != 0;
		ReleaseSRWLockExclusive(&gLogQueueLock);

		if (gLog) {
			const LONG dropped = InterlockedExchange(&gLogDroppedLines, 0);
			if (dropped > 0) {
				char droppedLine[256];
				snprintf(droppedLine, sizeof(droppedLine),
					"{\"func\":\"DX12LogQueue\",\"event\":\"DroppedLines\",\"count\":%ld}\n",
					dropped);
				fputs(droppedLine, gLog);
			}
			for (const auto &line : pending)
				fputs(line.c_str(), gLog);
			if (!pending.empty() || stopping)
				fflush(gLog);
		}

		if (stopping && pending.empty())
			break;
	}
	return 0;
}

bool DX12OpenLogFile()
{
#if !defined(_DEBUG)
	gReleaseStartupLogging.store(true, std::memory_order_relaxed);
#endif
	gDiagnosticsLogging.store(ReadDiagnosticsLoggingEnabled(), std::memory_order_relaxed);
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(gModule, path, MAX_PATH))
		return false;

	PathRemoveFileSpecW(path);
	PathAppendW(path, L"d3d12_log.jsonl");
	gLog = _wfsopen(path, L"w", _SH_DENYNO);
	if (!gLog)
		return false;

	InterlockedExchange(&gLogStopRequested, 0);
	gLogEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!gLogEvent) {
		fclose(gLog);
		gLog = nullptr;
		return false;
	}
	gLogThread = CreateThread(nullptr, 0, DX12LogThreadProc, nullptr, 0, nullptr);
	if (!gLogThread) {
		CloseHandle(gLogEvent);
		gLogEvent = nullptr;
		fclose(gLog);
		gLog = nullptr;
		return false;
	}
	return true;
}

#if !defined(_DEBUG)
void DX12EndReleaseStartupLogging()
{
	gReleaseStartupLogging.store(false, std::memory_order_relaxed);
}

static bool DX12ReleaseShouldLogFunction(const char *func)
{
	if (gReleaseStartupLogging.load(std::memory_order_relaxed))
		return true;
	if (!func)
		return false;
	if (DX12DiagnosticsLoggingEnabled())
		return true;
	return strcmp(func, "DX12ModRuntimeReload") == 0 ||
		strcmp(func, "DX12PreSkinMatchCsApply") == 0;
}
#else
void DX12EndReleaseStartupLogging()
{
}
#endif

static void MakeLogTime(char *buffer, size_t bufferSize)
{
	if (!buffer || !bufferSize)
		return;
	SYSTEMTIME localTime;
	GetLocalTime(&localTime);
	snprintf(buffer, bufferSize,
		"\"%02u:%02u:%02u.%03u\"",
		localTime.wHour, localTime.wMinute, localTime.wSecond,
		localTime.wMilliseconds);
}

static void EnqueueJsonLogLine(const char *line)
{
	if (!line || !line[0])
		return;

	AcquireSRWLockExclusive(&gLogQueueLock);
	if (gLogQueue.size() < kMaxQueuedLogLines) {
		gLogQueue.emplace_back(line);
	} else {
		InterlockedIncrement(&gLogDroppedLines);
	}
	ReleaseSRWLockExclusive(&gLogQueueLock);
	if (gLogEvent)
		SetEvent(gLogEvent);
}

static void WriteJsonLogLine(const char *func, const char *funcJson, const char *extra, bool flush)
{
	if (!gLog)
		return;

	char timeJson[64] = {};
	MakeLogTime(timeJson, sizeof(timeJson));
	const bool isHookCall = func && strcmp(func, "DX12HookCall") == 0;
	char line[8192];
	if (extra && extra[0]) {
		if (isHookCall) {
			snprintf(line, sizeof(line),
				"{%s,\"time\":%s}\n",
				extra, timeJson);
		} else {
			snprintf(line, sizeof(line),
				"{\"func\":%s,\"time\":%s,%s}\n",
				funcJson, timeJson, extra);
		}
	} else {
		if (isHookCall) {
			snprintf(line, sizeof(line),
				"{\"api\":\"\",\"time\":%s}\n",
				timeJson);
		} else {
			snprintf(line, sizeof(line),
				"{\"func\":%s,\"time\":%s}\n",
				funcJson, timeJson);
		}
	}
	EnqueueJsonLogLine(line);
	if (flush)
		DX12FlushLog();
}

void DX12CloseLogFile()
{
	InterlockedExchange(&gLogStopRequested, 1);
	if (gLogEvent)
		SetEvent(gLogEvent);
	if (gLogThread) {
		WaitForSingleObject(gLogThread, 5000);
		CloseHandle(gLogThread);
		gLogThread = nullptr;
	}
	if (gLogEvent) {
		CloseHandle(gLogEvent);
		gLogEvent = nullptr;
	}
	if (gLog) {
		fclose(gLog);
		gLog = nullptr;
	}
	AcquireSRWLockExclusive(&gLogQueueLock);
	gLogQueue.clear();
	ReleaseSRWLockExclusive(&gLogQueueLock);
}

void DX12Log(const char *fmt, ...)
{
	if (!gLog)
		return;
#if !defined(_DEBUG)
	if (!DX12ReleaseShouldLogFunction("Log"))
		return;
#endif

	va_list args;
	char message[2048];
	char fields[4096];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	fields[0] = '\0';
	DX12JsonAppendLogFieldsFromText(fields, sizeof(fields), message);
	char funcJson[16];
	DX12JsonEscapeString(funcJson, sizeof(funcJson), "Log");
	WriteJsonLogLine("Log", funcJson, fields[0] == ',' ? fields + 1 : fields, false);
}

void DX12LogJsonFunc(const char *func, const char *fmt, ...)
{
	if (!gLog)
		return;
#if !defined(_DEBUG)
	if (!DX12ReleaseShouldLogFunction(func))
		return;
#endif

	char funcJson[512];
	char extra[4096];
	DX12JsonEscapeString(funcJson, sizeof(funcJson), func ? func : "Unknown");
	extra[0] = '\0';

	if (fmt && fmt[0]) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(extra, sizeof(extra), fmt, args);
		va_end(args);
	}

	WriteJsonLogLine(func ? func : "Unknown", funcJson, extra, false);
}

void DX12LogJsonFuncFlush(const char *func, const char *fmt, ...)
{
	if (!gLog)
		return;
#if !defined(_DEBUG)
	if (!DX12ReleaseShouldLogFunction(func))
		return;
#endif

	char funcJson[512];
	char extra[4096];
	DX12JsonEscapeString(funcJson, sizeof(funcJson), func ? func : "Unknown");
	extra[0] = '\0';

	if (fmt && fmt[0]) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(extra, sizeof(extra), fmt, args);
		va_end(args);
	}

	WriteJsonLogLine(func ? func : "Unknown", funcJson, extra, true);
}

#if defined(_DEBUG)
void DX12LogDebugJsonFunc(const char *func, const char *fmt, ...)
{
	if (!gLog)
		return;
	if (!DX12DiagnosticsLoggingEnabled())
		return;

	char funcJson[512];
	char extra[4096];
	DX12JsonEscapeString(funcJson, sizeof(funcJson), func ? func : "Unknown");
	extra[0] = '\0';

	if (fmt && fmt[0]) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(extra, sizeof(extra), fmt, args);
		va_end(args);
	}

	WriteJsonLogLine(func ? func : "Unknown", funcJson, extra, false);
}

void DX12LogDebugJsonFuncFlush(const char *func, const char *fmt, ...)
{
	if (!gLog)
		return;
	if (!DX12DiagnosticsLoggingEnabled())
		return;

	char funcJson[512];
	char extra[4096];
	DX12JsonEscapeString(funcJson, sizeof(funcJson), func ? func : "Unknown");
	extra[0] = '\0';

	if (fmt && fmt[0]) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(extra, sizeof(extra), fmt, args);
		va_end(args);
	}

	WriteJsonLogLine(func ? func : "Unknown", funcJson, extra, true);
}
#endif

void DX12FlushLog()
{
	if (gLogEvent)
		SetEvent(gLogEvent);
}

static bool RememberHook(void *target)
{
	AcquireSRWLockExclusive(&gStateLock);
	bool inserted = gHookedFunctions.insert(target).second;
	ReleaseSRWLockExclusive(&gStateLock);
	return inserted;
}

static void RememberOriginal(void *target, void *original)
{
	if (!target || !original)
		return;

	AcquireSRWLockExclusive(&gStateLock);
	gOriginalFunctions[target] = original;
	ReleaseSRWLockExclusive(&gStateLock);
}

DWORD DX12HookFunction(void **original, void *target, void *hook, const char *name)
{
	if (!target || !hook)
		return ERROR_PROC_NOT_FOUND;
	if (!RememberHook(target))
		return ERROR_SUCCESS;

	SIZE_T hookId = 0;
	DWORD result = gHookMgr.Hook(&hookId, original, target, hook);
	if (result == ERROR_SUCCESS && original)
		RememberOriginal(target, *original);
	DX12LogJsonFunc(name ? name : "HookInstall",
		"\"event\":\"HookInstall\",\"status\":\"%s\",\"target\":\"%p\",\"original\":\"%p\",\"result\":\"0x%lx\"",
		result == ERROR_SUCCESS ? "installed" : "failed",
		target, original ? *original : nullptr, result);
	return result;
}

void *DX12GetOriginalFunction(void *target)
{
	if (!target)
		return nullptr;

	AcquireSRWLockShared(&gStateLock);
	auto it = gOriginalFunctions.find(target);
	void *original = it != gOriginalFunctions.end() ? it->second : nullptr;
	ReleaseSRWLockShared(&gStateLock);
	return original;
}

void DX12UnhookAll()
{
	gHookMgr.UnhookAll();
}

LONG DX12IncrementPresentCount()
{
	return InterlockedIncrement(&gPresentCount);
}

LONG DX12GetPresentCount()
{
	return gPresentCount;
}

void DX12SetOverlayWindow(HWND hwnd)
{
	gOverlayWindow = hwnd;
}

HWND DX12GetOverlayWindow()
{
	return gOverlayWindow;
}

static void SetOverlayStatusColorLocked(const wchar_t *status, COLORREF color)
{
	if (status && status[0])
		wcsncpy_s(gOverlayStatus, status, _TRUNCATE);
	else
		wcsncpy_s(gOverlayStatus, L"3DMigoto DX12 hook alive", _TRUNCATE);
	gOverlayStatusColor = color;
	gOverlayStatusExpireTick = 0;
}

static void InvalidateOverlayStatus()
{
	HWND hwnd = DX12GetOverlayWindow();
	if (hwnd)
		InvalidateRect(hwnd, nullptr, FALSE);
}

void DX12SetOverlayStatus(const wchar_t *status)
{
	AcquireSRWLockExclusive(&gStateLock);
	SetOverlayStatusColorLocked(status, RGB(0, 255, 0));
	ReleaseSRWLockExclusive(&gStateLock);
	InvalidateOverlayStatus();
}

void DX12SetOverlayWarning(const wchar_t *status)
{
	AcquireSRWLockExclusive(&gStateLock);
	SetOverlayStatusColorLocked(status, RGB(255, 220, 0));
	ReleaseSRWLockExclusive(&gStateLock);
	InvalidateOverlayStatus();
}

void DX12SetOverlayError(const wchar_t *status)
{
	AcquireSRWLockExclusive(&gStateLock);
	SetOverlayStatusColorLocked(status, RGB(255, 64, 64));
	ReleaseSRWLockExclusive(&gStateLock);
	InvalidateOverlayStatus();
}

void DX12SetOverlayStatusTemporary(const wchar_t *status, DWORD durationMs)
{
	AcquireSRWLockExclusive(&gStateLock);
	if (status && status[0])
		wcsncpy_s(gOverlayStatus, status, _TRUNCATE);
	else
		wcsncpy_s(gOverlayStatus, L"3DMigoto DX12 hook alive", _TRUNCATE);
	gOverlayStatusColor = RGB(0, 255, 0);
	gOverlayStatusExpireTick = durationMs ? GetTickCount() + durationMs : 0;
	ReleaseSRWLockExclusive(&gStateLock);
	InvalidateOverlayStatus();
}

void DX12ClearExpiredOverlayStatus()
{
	bool changed = false;
	AcquireSRWLockExclusive(&gStateLock);
	if (gOverlayStatusExpireTick) {
		const DWORD now = GetTickCount();
		if (static_cast<LONG>(now - gOverlayStatusExpireTick) >= 0) {
			wcsncpy_s(gOverlayStatus, L"3DMigoto DX12 hook alive", _TRUNCATE);
			gOverlayStatusColor = RGB(0, 255, 0);
			gOverlayStatusExpireTick = 0;
			changed = true;
		}
	}
	ReleaseSRWLockExclusive(&gStateLock);
	if (changed)
		InvalidateOverlayStatus();
}

void DX12GetOverlayStatus(wchar_t *status, size_t statusCount)
{
	if (!status || statusCount == 0)
		return;

	AcquireSRWLockShared(&gStateLock);
	wcsncpy_s(status, statusCount, gOverlayStatus, _TRUNCATE);
	ReleaseSRWLockShared(&gStateLock);
}

COLORREF DX12GetOverlayStatusColor()
{
	AcquireSRWLockShared(&gStateLock);
	COLORREF color = gOverlayStatusColor;
	ReleaseSRWLockShared(&gStateLock);
	return color;
}

void DX12SetCommandQueue(ID3D12CommandQueue *queue)
{
	if (queue) {
		D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
		if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
			DX12Log("Ignoring non-direct command queue for frame analysis readback: queue=%p type=%d\n",
				queue, static_cast<int>(desc.Type));
			return;
		}
	}

	if (queue)
		queue->AddRef();

	AcquireSRWLockExclusive(&gStateLock);
	ID3D12CommandQueue *oldQueue = gCommandQueue;
	gCommandQueue = queue;
	ReleaseSRWLockExclusive(&gStateLock);

	if (oldQueue)
		oldQueue->Release();
}

ID3D12CommandQueue *DX12AcquireCommandQueue()
{
	AcquireSRWLockShared(&gStateLock);
	ID3D12CommandQueue *queue = gCommandQueue;
	if (queue)
		queue->AddRef();
	ReleaseSRWLockShared(&gStateLock);
	return queue;
}
