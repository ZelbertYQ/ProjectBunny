#include "D3D12Wrapper.h"

#include <Shlwapi.h>
#include <stdio.h>
#include <unordered_map>
#include <unordered_set>

#include "Nektra/NktHookLib.h"

static HINSTANCE gModule = nullptr;
static HMODULE gRealD3D12 = nullptr;
static FILE *gLog = nullptr;
static bool gInitialized = false;
static bool gDXGIHookAttempted = false;
static SRWLOCK gStateLock = SRWLOCK_INIT;
static CNktHookLib gHookMgr;
static std::unordered_set<void*> gHookedFunctions;
static std::unordered_map<IDXGISwapChain*, HWND> gSwapChainWindows;

typedef HRESULT(WINAPI *PFN_D3D12_CREATE_DEVICE_LOCAL)(
	IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT(WINAPI *PFN_D3D12_GET_DEBUG_INTERFACE_LOCAL)(REFIID, void**);
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_ROOT_SIGNATURE_LOCAL)(
	const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI *PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER_LOCAL)(
	LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE_LOCAL)(
	const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI *PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER_LOCAL)(
	LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI *PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL)(
	UINT, const IID*, void*, UINT*);
typedef HRESULT(WINAPI *PFN_D3D12_GET_INTERFACE_LOCAL)(REFCLSID, REFIID, void**);

static PFN_D3D12_CREATE_DEVICE_LOCAL gOrigD3D12CreateDevice = nullptr;
static PFN_D3D12_GET_DEBUG_INTERFACE_LOCAL gOrigD3D12GetDebugInterface = nullptr;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE_LOCAL gOrigD3D12SerializeRootSignature = nullptr;
static PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER_LOCAL gOrigD3D12CreateRootSignatureDeserializer = nullptr;
static PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE_LOCAL gOrigD3D12SerializeVersionedRootSignature = nullptr;
static PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER_LOCAL gOrigD3D12CreateVersionedRootSignatureDeserializer = nullptr;
static PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL gOrigD3D12EnableExperimentalFeatures = nullptr;
static PFN_D3D12_GET_INTERFACE_LOCAL gOrigD3D12GetInterface = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_SWAP_CHAIN)(
	IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_SWAP_CHAIN_FOR_HWND)(
	IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_SWAP_CHAIN_FOR_CORE_WINDOW)(
	IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
	IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_SWAP_CHAIN_FOR_COMPOSITION)(
	IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_PRESENT)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_PRESENT1)(
	IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

static PFN_CREATE_SWAP_CHAIN gOrigCreateSwapChain = nullptr;
static PFN_CREATE_SWAP_CHAIN_FOR_HWND gOrigCreateSwapChainForHwnd = nullptr;
static PFN_CREATE_SWAP_CHAIN_FOR_CORE_WINDOW gOrigCreateSwapChainForCoreWindow = nullptr;
static PFN_CREATE_SWAP_CHAIN_FOR_COMPOSITION gOrigCreateSwapChainForComposition = nullptr;
static PFN_PRESENT gOrigPresent = nullptr;
static PFN_PRESENT1 gOrigPresent1 = nullptr;

typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY)(REFIID, void**);
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT, REFIID, void**);

static PFN_CREATE_DXGI_FACTORY gOrigCreateDXGIFactory = nullptr;
static PFN_CREATE_DXGI_FACTORY gOrigCreateDXGIFactory1 = nullptr;
static PFN_CREATE_DXGI_FACTORY2 gOrigCreateDXGIFactory2 = nullptr;

static HRESULT WINAPI HookedCreateDXGIFactory(REFIID riid, void **factory);
static HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void **factory);
static HRESULT WINAPI HookedCreateDXGIFactory2(UINT flags, REFIID riid, void **factory);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
	IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapChain);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
	IDXGIFactory2 *factory, IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreenDesc, IDXGIOutput *output, IDXGISwapChain1 **swapChain);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForCoreWindow(
	IDXGIFactory2 *factory, IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc,
	IDXGIOutput *output, IDXGISwapChain1 **swapChain);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForComposition(
	IDXGIFactory2 *factory, IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc,
	IDXGIOutput *output, IDXGISwapChain1 **swapChain);
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain *swapChain, UINT syncInterval, UINT flags);
static HRESULT STDMETHODCALLTYPE HookedPresent1(
	IDXGISwapChain1 *swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS *presentParameters);

static void DX12Log(const char *fmt, ...)
{
	if (!gLog)
		return;

	va_list args;
	va_start(args, fmt);
	vfprintf(gLog, fmt, args);
	va_end(args);
	fflush(gLog);
}

static HMODULE LoadRealD3D12()
{
	if (gRealD3D12)
		return gRealD3D12;

	wchar_t path[MAX_PATH];
	if (!GetSystemDirectoryW(path, MAX_PATH))
		return nullptr;
	PathAppendW(path, L"d3d12.dll");

	gRealD3D12 = LoadLibraryW(path);
	if (!gRealD3D12) {
		DX12Log("Failed to load real d3d12.dll from %S, error=%lu\n", path, GetLastError());
		return nullptr;
	}

	gOrigD3D12CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE_LOCAL>(
		GetProcAddress(gRealD3D12, "D3D12CreateDevice"));
	gOrigD3D12GetDebugInterface = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE_LOCAL>(
		GetProcAddress(gRealD3D12, "D3D12GetDebugInterface"));
	gOrigD3D12SerializeRootSignature = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE_LOCAL>(
		GetProcAddress(gRealD3D12, "D3D12SerializeRootSignature"));
	gOrigD3D12CreateRootSignatureDeserializer =
		reinterpret_cast<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER_LOCAL>(
			GetProcAddress(gRealD3D12, "D3D12CreateRootSignatureDeserializer"));
	gOrigD3D12SerializeVersionedRootSignature =
		reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE_LOCAL>(
			GetProcAddress(gRealD3D12, "D3D12SerializeVersionedRootSignature"));
	gOrigD3D12CreateVersionedRootSignatureDeserializer =
		reinterpret_cast<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER_LOCAL>(
			GetProcAddress(gRealD3D12, "D3D12CreateVersionedRootSignatureDeserializer"));
	gOrigD3D12EnableExperimentalFeatures =
		reinterpret_cast<PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL>(
			GetProcAddress(gRealD3D12, "D3D12EnableExperimentalFeatures"));
	gOrigD3D12GetInterface = reinterpret_cast<PFN_D3D12_GET_INTERFACE_LOCAL>(
		GetProcAddress(gRealD3D12, "D3D12GetInterface"));

	DX12Log("Loaded real d3d12.dll: %p\n", gRealD3D12);
	return gRealD3D12;
}

static bool RememberHook(void *target)
{
	AcquireSRWLockExclusive(&gStateLock);
	bool inserted = gHookedFunctions.insert(target).second;
	ReleaseSRWLockExclusive(&gStateLock);
	return inserted;
}

static DWORD HookFunction(void **original, void *target, void *hook, const char *name)
{
	if (!target || !hook)
		return ERROR_PROC_NOT_FOUND;
	if (!RememberHook(target))
		return ERROR_SUCCESS;

	SIZE_T hookId = 0;
	DWORD result = gHookMgr.Hook(&hookId, original, target, hook);
	DX12Log("%s hook %s target=%p original=%p result=0x%lx\n",
		result == ERROR_SUCCESS ? "Installed" : "Failed",
		name, target, original ? *original : nullptr, result);
	return result;
}

static void RegisterSwapChainWindow(IDXGISwapChain *swapChain, HWND window)
{
	if (!swapChain)
		return;

	if (!window) {
		DXGI_SWAP_CHAIN_DESC desc = {};
		if (SUCCEEDED(swapChain->GetDesc(&desc)))
			window = desc.OutputWindow;
	}

	AcquireSRWLockExclusive(&gStateLock);
	gSwapChainWindows[swapChain] = window;
	ReleaseSRWLockExclusive(&gStateLock);

	DX12Log("Registered swapchain %p hwnd=%p\n", swapChain, window);
}

static HWND LookupSwapChainWindow(IDXGISwapChain *swapChain)
{
	HWND window = nullptr;

	AcquireSRWLockShared(&gStateLock);
	auto found = gSwapChainWindows.find(swapChain);
	if (found != gSwapChainWindows.end())
		window = found->second;
	ReleaseSRWLockShared(&gStateLock);

	if (!window) {
		DXGI_SWAP_CHAIN_DESC desc = {};
		if (swapChain && SUCCEEDED(swapChain->GetDesc(&desc)))
			window = desc.OutputWindow;
	}

	return window;
}

static void DrawGreenText(IDXGISwapChain *swapChain)
{
	HWND window = LookupSwapChainWindow(swapChain);
	if (!window || !IsWindow(window))
		return;

	HDC dc = GetDC(window);
	if (!dc)
		return;

	int oldBkMode = SetBkMode(dc, TRANSPARENT);
	COLORREF oldTextColor = SetTextColor(dc, RGB(0, 255, 0));
	HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
	const wchar_t text[] = L"3DMigoto DX12 hook alive";

	TextOutW(dc, 20, 20, text, ARRAYSIZE(text) - 1);

	if (oldFont)
		SelectObject(dc, oldFont);
	SetTextColor(dc, oldTextColor);
	SetBkMode(dc, oldBkMode);
	ReleaseDC(window, dc);
}

static void HookSwapChain(IDXGISwapChain *swapChain)
{
	if (!swapChain)
		return;

	void **vtable = *reinterpret_cast<void***>(swapChain);
	HookFunction(reinterpret_cast<void**>(&gOrigPresent), vtable[8], HookedPresent, "IDXGISwapChain::Present");

	IDXGISwapChain1 *swapChain1 = nullptr;
	if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1)))) {
		void **vtable1 = *reinterpret_cast<void***>(swapChain1);
		HookFunction(reinterpret_cast<void**>(&gOrigPresent1), vtable1[22], HookedPresent1, "IDXGISwapChain1::Present1");
		swapChain1->Release();
	}
}

static void HookFactory(IUnknown *factory)
{
	if (!factory)
		return;

	IDXGIFactory *factory0 = nullptr;
	if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory0)))) {
		void **vtable = *reinterpret_cast<void***>(factory0);
		HookFunction(reinterpret_cast<void**>(&gOrigCreateSwapChain),
			vtable[10], HookedCreateSwapChain, "IDXGIFactory::CreateSwapChain");
		factory0->Release();
	}

	IDXGIFactory2 *factory2 = nullptr;
	if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
		void **vtable = *reinterpret_cast<void***>(factory2);
		HookFunction(reinterpret_cast<void**>(&gOrigCreateSwapChainForHwnd),
			vtable[15], HookedCreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
		HookFunction(reinterpret_cast<void**>(&gOrigCreateSwapChainForCoreWindow),
			vtable[16], HookedCreateSwapChainForCoreWindow, "IDXGIFactory2::CreateSwapChainForCoreWindow");
		HookFunction(reinterpret_cast<void**>(&gOrigCreateSwapChainForComposition),
			vtable[24], HookedCreateSwapChainForComposition, "IDXGIFactory2::CreateSwapChainForComposition");
		factory2->Release();
	}
}

static void HookDXGIFactoryVTables(HMODULE dxgi)
{
	auto createFactory = reinterpret_cast<PFN_CREATE_DXGI_FACTORY>(
		GetProcAddress(dxgi, "CreateDXGIFactory"));
	auto createFactory1 = reinterpret_cast<PFN_CREATE_DXGI_FACTORY>(
		GetProcAddress(dxgi, "CreateDXGIFactory1"));
	auto createFactory2 = reinterpret_cast<PFN_CREATE_DXGI_FACTORY2>(
		GetProcAddress(dxgi, "CreateDXGIFactory2"));

	IDXGIFactory *factory = nullptr;
	if (createFactory1 && SUCCEEDED(createFactory1(IID_PPV_ARGS(&factory)))) {
		HookFactory(factory);
		factory->Release();
		return;
	}

	if (createFactory && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory)))) {
		HookFactory(factory);
		factory->Release();
	}

	IDXGIFactory2 *factory2 = nullptr;
	if (createFactory2 && SUCCEEDED(createFactory2(0, IID_PPV_ARGS(&factory2)))) {
		HookFactory(factory2);
		factory2->Release();
	}
}

static HRESULT WINAPI HookedCreateDXGIFactory(REFIID riid, void **factory)
{
	HRESULT hr = gOrigCreateDXGIFactory(riid, factory);
	DX12Log("CreateDXGIFactory riid=%p result=0x%lx factory=%p\n", &riid, hr, factory ? *factory : nullptr);
	if (SUCCEEDED(hr) && factory)
		HookFactory(static_cast<IUnknown*>(*factory));
	return hr;
}

static HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void **factory)
{
	HRESULT hr = gOrigCreateDXGIFactory1(riid, factory);
	DX12Log("CreateDXGIFactory1 riid=%p result=0x%lx factory=%p\n", &riid, hr, factory ? *factory : nullptr);
	if (SUCCEEDED(hr) && factory)
		HookFactory(static_cast<IUnknown*>(*factory));
	return hr;
}

static HRESULT WINAPI HookedCreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
	HRESULT hr = gOrigCreateDXGIFactory2(flags, riid, factory);
	DX12Log("CreateDXGIFactory2 flags=0x%x riid=%p result=0x%lx factory=%p\n",
		flags, &riid, hr, factory ? *factory : nullptr);
	if (SUCCEEDED(hr) && factory)
		HookFactory(static_cast<IUnknown*>(*factory));
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
	IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapChain)
{
	HRESULT hr = gOrigCreateSwapChain(factory, device, desc, swapChain);
	DX12Log("CreateSwapChain device=%p result=0x%lx swapchain=%p\n",
		device, hr, swapChain ? *swapChain : nullptr);
	if (SUCCEEDED(hr) && swapChain) {
		RegisterSwapChainWindow(*swapChain, desc ? desc->OutputWindow : nullptr);
		HookSwapChain(*swapChain);
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
	IDXGIFactory2 *factory, IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreenDesc, IDXGIOutput *output, IDXGISwapChain1 **swapChain)
{
	HRESULT hr = gOrigCreateSwapChainForHwnd(factory, device, window, desc, fullscreenDesc, output, swapChain);
	DX12Log("CreateSwapChainForHwnd device=%p hwnd=%p result=0x%lx swapchain=%p\n",
		device, window, hr, swapChain ? *swapChain : nullptr);
	if (SUCCEEDED(hr) && swapChain) {
		RegisterSwapChainWindow(*swapChain, window);
		HookSwapChain(*swapChain);
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForCoreWindow(
	IDXGIFactory2 *factory, IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc,
	IDXGIOutput *output, IDXGISwapChain1 **swapChain)
{
	HRESULT hr = gOrigCreateSwapChainForCoreWindow(factory, device, window, desc, output, swapChain);
	DX12Log("CreateSwapChainForCoreWindow device=%p result=0x%lx swapchain=%p\n",
		device, hr, swapChain ? *swapChain : nullptr);
	if (SUCCEEDED(hr) && swapChain)
		HookSwapChain(*swapChain);
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForComposition(
	IDXGIFactory2 *factory, IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc,
	IDXGIOutput *output, IDXGISwapChain1 **swapChain)
{
	HRESULT hr = gOrigCreateSwapChainForComposition(factory, device, desc, output, swapChain);
	DX12Log("CreateSwapChainForComposition device=%p result=0x%lx swapchain=%p\n",
		device, hr, swapChain ? *swapChain : nullptr);
	if (SUCCEEDED(hr) && swapChain)
		HookSwapChain(*swapChain);
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain *swapChain, UINT syncInterval, UINT flags)
{
	HRESULT hr = gOrigPresent(swapChain, syncInterval, flags);
	DrawGreenText(swapChain);
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent1(
	IDXGISwapChain1 *swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS *presentParameters)
{
	HRESULT hr = gOrigPresent1(swapChain, syncInterval, flags, presentParameters);
	DrawGreenText(swapChain);
	return hr;
}

void DX12InstallDXGIHooks()
{
	AcquireSRWLockExclusive(&gStateLock);
	if (gDXGIHookAttempted) {
		ReleaseSRWLockExclusive(&gStateLock);
		return;
	}
	gDXGIHookAttempted = true;
	ReleaseSRWLockExclusive(&gStateLock);

	HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
	if (!dxgi) {
		DX12Log("dxgi.dll is not loaded yet; DXGI hook deferred\n");
		AcquireSRWLockExclusive(&gStateLock);
		gDXGIHookAttempted = false;
		ReleaseSRWLockExclusive(&gStateLock);
		return;
	}

	HookDXGIFactoryVTables(dxgi);

	HookFunction(reinterpret_cast<void**>(&gOrigCreateDXGIFactory),
		GetProcAddress(dxgi, "CreateDXGIFactory"), HookedCreateDXGIFactory, "CreateDXGIFactory");
	HookFunction(reinterpret_cast<void**>(&gOrigCreateDXGIFactory1),
		GetProcAddress(dxgi, "CreateDXGIFactory1"), HookedCreateDXGIFactory1, "CreateDXGIFactory1");
	HookFunction(reinterpret_cast<void**>(&gOrigCreateDXGIFactory2),
		GetProcAddress(dxgi, "CreateDXGIFactory2"), HookedCreateDXGIFactory2, "CreateDXGIFactory2");
}

static DWORD WINAPI DX12WorkerThread(void*)
{
	DX12Log("DX12 worker thread started\n");
	LoadRealD3D12();

	for (int i = 0; i < 200; ++i) {
		DX12InstallDXGIHooks();
		Sleep(50);

		AcquireSRWLockShared(&gStateLock);
		bool installed = gDXGIHookAttempted;
		ReleaseSRWLockShared(&gStateLock);
		if (installed)
			break;
	}

	return 0;
}

void DX12Initialize(HINSTANCE module)
{
	if (gInitialized)
		return;
	gInitialized = true;
	gModule = module;

	wchar_t path[MAX_PATH];
	if (GetModuleFileNameW(module, path, MAX_PATH)) {
		PathRemoveFileSpecW(path);
		PathAppendW(path, L"d3d12_log.txt");
		gLog = _wfsopen(path, L"w", _SH_DENYNO);
	}

	DX12Log("\n*** 3DMigoto DX12 proxy initialized ***\n");

	HANDLE thread = CreateThread(nullptr, 0, DX12WorkerThread, nullptr, 0, nullptr);
	if (thread)
		CloseHandle(thread);
}

void DX12Shutdown()
{
	DX12Log("*** 3DMigoto DX12 proxy shutting down ***\n");
	gHookMgr.UnhookAll();

	if (gRealD3D12) {
		FreeLibrary(gRealD3D12);
		gRealD3D12 = nullptr;
	}

	if (gLog) {
		fclose(gLog);
		gLog = nullptr;
	}
}

extern "C" HRESULT WINAPI D3D12CreateDevice(
	IUnknown *adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void **device)
{
	LoadRealD3D12();
	DX12InstallDXGIHooks();
	DX12Log("D3D12CreateDevice adapter=%p minFeature=0x%x\n", adapter, minimumFeatureLevel);
	if (!gOrigD3D12CreateDevice)
		return E_FAIL;
	return gOrigD3D12CreateDevice(adapter, minimumFeatureLevel, riid, device);
}

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **debug)
{
	LoadRealD3D12();
	if (!gOrigD3D12GetDebugInterface)
		return E_FAIL;
	return gOrigD3D12GetDebugInterface(riid, debug);
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
	const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version,
	ID3DBlob **blob, ID3DBlob **errorBlob)
{
	LoadRealD3D12();
	if (!gOrigD3D12SerializeRootSignature)
		return E_FAIL;
	return gOrigD3D12SerializeRootSignature(desc, version, blob, errorBlob);
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
	LPCVOID srcData, SIZE_T srcDataSize, REFIID riid, void **rootSignatureDeserializer)
{
	LoadRealD3D12();
	if (!gOrigD3D12CreateRootSignatureDeserializer)
		return E_FAIL;
	return gOrigD3D12CreateRootSignatureDeserializer(
		srcData, srcDataSize, riid, rootSignatureDeserializer);
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
	const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob, ID3DBlob **errorBlob)
{
	LoadRealD3D12();
	if (!gOrigD3D12SerializeVersionedRootSignature)
		return E_FAIL;
	return gOrigD3D12SerializeVersionedRootSignature(desc, blob, errorBlob);
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
	LPCVOID srcData, SIZE_T srcDataSize, REFIID riid, void **rootSignatureDeserializer)
{
	LoadRealD3D12();
	if (!gOrigD3D12CreateVersionedRootSignatureDeserializer)
		return E_FAIL;
	return gOrigD3D12CreateVersionedRootSignatureDeserializer(
		srcData, srcDataSize, riid, rootSignatureDeserializer);
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
	UINT numFeatures, const IID *featureIIDs, void *configurationStructs, UINT *configurationStructSizes)
{
	LoadRealD3D12();
	if (!gOrigD3D12EnableExperimentalFeatures)
		return E_FAIL;
	return gOrigD3D12EnableExperimentalFeatures(
		numFeatures, featureIIDs, configurationStructs, configurationStructSizes);
}

extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void **object)
{
	LoadRealD3D12();
	if (!gOrigD3D12GetInterface)
		return E_NOINTERFACE;
	return gOrigD3D12GetInterface(clsid, riid, object);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(module);
		DX12Initialize(module);
		break;
	case DLL_PROCESS_DETACH:
		DX12Shutdown();
		break;
	}
	return TRUE;
}
