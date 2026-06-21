#include "DX12State.h"

#include <Shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <unordered_map>
#include <unordered_set>

#include "DX12Json.h"
#include "Nektra/NktHookLib.h"

static HINSTANCE gModule = nullptr;
static FILE *gLog = nullptr;
static unsigned long long gLogLineNo = 0;
static volatile LONG gPresentCount = 0;
static HWND gOverlayWindow = nullptr;
static wchar_t gOverlayStatus[512] = L"3DMigoto DX12 hook alive";
static ID3D12CommandQueue *gCommandQueue = nullptr;
static SRWLOCK gStateLock = SRWLOCK_INIT;
static CNktHookLib gHookMgr;
static std::unordered_set<void*> gHookedFunctions;
static std::unordered_map<void*, void*> gOriginalFunctions;

void DX12SetModule(HINSTANCE module)
{
	gModule = module;
}

HINSTANCE DX12GetModule()
{
	return gModule;
}

bool DX12OpenLogFile()
{
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(gModule, path, MAX_PATH))
		return false;

	PathRemoveFileSpecW(path);
	PathAppendW(path, L"d3d12_log.jsonl");
	gLog = _wfsopen(path, L"w", _SH_DENYNO);
	gLogLineNo = 0;
	return gLog != nullptr;
}

void DX12CloseLogFile()
{
	if (gLog) {
		fclose(gLog);
		gLog = nullptr;
	}
}

void DX12Log(const char *fmt, ...)
{
	if (!gLog)
		return;

	va_list args;
	char message[2048];
	char fields[4096];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	strcpy_s(fields, sizeof(fields), "\"func\":\"Log\"");
	DX12JsonAppendLogFieldsFromText(fields, sizeof(fields), message);
	fprintf(gLog, "{\"index\":%llu,%s}\n", ++gLogLineNo, fields);
	fflush(gLog);
}

void DX12LogJsonFunc(const char *func, const char *fmt, ...)
{
	if (!gLog)
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

	if (extra[0])
		fprintf(gLog, "{\"index\":%llu,\"func\":%s,%s}\n", ++gLogLineNo, funcJson, extra);
	else
		fprintf(gLog, "{\"index\":%llu,\"func\":%s}\n", ++gLogLineNo, funcJson);
	fflush(gLog);
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

void DX12SetOverlayStatus(const wchar_t *status)
{
	AcquireSRWLockExclusive(&gStateLock);
	if (status && status[0])
		wcsncpy_s(gOverlayStatus, status, _TRUNCATE);
	else
		wcsncpy_s(gOverlayStatus, L"3DMigoto DX12 hook alive", _TRUNCATE);
	ReleaseSRWLockExclusive(&gStateLock);

	HWND hwnd = DX12GetOverlayWindow();
	if (hwnd)
		InvalidateRect(hwnd, nullptr, FALSE);
}

void DX12GetOverlayStatus(wchar_t *status, size_t statusCount)
{
	if (!status || statusCount == 0)
		return;

	AcquireSRWLockShared(&gStateLock);
	wcsncpy_s(status, statusCount, gOverlayStatus, _TRUNCATE);
	ReleaseSRWLockShared(&gStateLock);
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
