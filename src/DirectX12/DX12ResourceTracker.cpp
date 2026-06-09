#include "DX12ResourceTracker.h"

#include <Shlwapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "DX12State.h"

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_DESCRIPTOR_HEAP)(
	ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_ROOT_SIGNATURE)(
	ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_CONSTANT_BUFFER_VIEW)(
	ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_SHADER_RESOURCE_VIEW)(
	ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_UNORDERED_ACCESS_VIEW)(
	ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_RENDER_TARGET_VIEW)(
	ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_DEPTH_STENCIL_VIEW)(
	ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_SAMPLER)(
	ID3D12Device*, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_COPY_DESCRIPTORS)(
	ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
	UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
typedef void(STDMETHODCALLTYPE *PFN_COPY_DESCRIPTORS_SIMPLE)(
	ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
	D3D12_DESCRIPTOR_HEAP_TYPE);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMITTED_RESOURCE)(
	ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
	const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
	REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_PLACED_RESOURCE)(
	ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
	D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_RESERVED_RESOURCE)(
	ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
	const D3D12_CLEAR_VALUE*, REFIID, void**);

static PFN_CREATE_DESCRIPTOR_HEAP gOrigCreateDescriptorHeap = nullptr;
static PFN_CREATE_ROOT_SIGNATURE gOrigCreateRootSignature = nullptr;
static PFN_CREATE_CONSTANT_BUFFER_VIEW gOrigCreateConstantBufferView = nullptr;
static PFN_CREATE_SHADER_RESOURCE_VIEW gOrigCreateShaderResourceView = nullptr;
static PFN_CREATE_UNORDERED_ACCESS_VIEW gOrigCreateUnorderedAccessView = nullptr;
static PFN_CREATE_RENDER_TARGET_VIEW gOrigCreateRenderTargetView = nullptr;
static PFN_CREATE_DEPTH_STENCIL_VIEW gOrigCreateDepthStencilView = nullptr;
static PFN_CREATE_SAMPLER gOrigCreateSampler = nullptr;
static PFN_COPY_DESCRIPTORS gOrigCopyDescriptors = nullptr;
static PFN_COPY_DESCRIPTORS_SIMPLE gOrigCopyDescriptorsSimple = nullptr;
static PFN_CREATE_COMMITTED_RESOURCE gOrigCreateCommittedResource = nullptr;
static PFN_CREATE_PLACED_RESOURCE gOrigCreatePlacedResource = nullptr;
static PFN_CREATE_RESERVED_RESOURCE gOrigCreateReservedResource = nullptr;

struct RootSignatureRecord
{
	ID3D12RootSignature *rootSignature = nullptr;
	UINT64 hash = 0;
	SIZE_T size = 0;
	UINT nodeMask = 0;
};

struct DescriptorHeapRecord
{
	ID3D12DescriptorHeap *heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	SIZE_T cpuStart = 0;
	UINT64 gpuStart = 0;
	UINT increment = 0;
};

struct DescriptorRecord
{
	std::string kind;
	SIZE_T cpuHandle = 0;
	ID3D12Resource *resource = nullptr;
	ID3D12Resource *counterResource = nullptr;
	D3D12_RESOURCE_DESC resourceDesc = {};
	bool hasResourceDesc = false;
	UINT resourceHeapType = 0;
	bool hasResourceHeapType = false;
	UINT64 gpuVirtualAddress = 0;
	UINT64 resourceOffset = 0;
	UINT64 viewSize = 0;
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
	D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
	D3D12_SAMPLER_DESC sampler = {};
	bool hasDesc = false;
};

struct ResourceRecord
{
	ID3D12Resource *resource = nullptr;
	D3D12_RESOURCE_DESC desc = {};
	D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
	bool hasHeapType = false;
	UINT64 gpuVirtualAddress = 0;
	UINT64 size = 0;
};

struct PsoRootRecord
{
	UINT64 psoIndex = 0;
	std::string kind;
	ID3D12PipelineState *pipelineState = nullptr;
	ID3D12RootSignature *rootSignature = nullptr;
};

static SRWLOCK gResourceLock = SRWLOCK_INIT;
static std::vector<RootSignatureRecord> gRootSignatures;
static std::vector<DescriptorHeapRecord> gDescriptorHeaps;
static std::vector<DescriptorRecord> gDescriptors;
static std::unordered_map<SIZE_T, size_t> gDescriptorByCpuHandle;
static std::vector<PsoRootRecord> gPsoRoots;
static std::vector<ResourceRecord> gResources;
static std::unordered_map<ID3D12Resource*, size_t> gResourceByPtr;
static bool gCleanupRegistered = false;

static UINT64 Fnv1a64(const void *data, size_t size)
{
	const uint8_t *bytes = static_cast<const uint8_t*>(data);
	UINT64 hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

static const char *DescriptorHeapTypeName(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	switch (type) {
	case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
		return "CBV_SRV_UAV";
	case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
		return "SAMPLER";
	case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
		return "RTV";
	case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
		return "DSV";
	default:
		return "UNKNOWN";
	}
}

static const char *ResourceDimensionName(D3D12_RESOURCE_DIMENSION dimension)
{
	switch (dimension) {
	case D3D12_RESOURCE_DIMENSION_BUFFER:
		return "BUFFER";
	case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
		return "TEXTURE1D";
	case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
		return "TEXTURE2D";
	case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
		return "TEXTURE3D";
	default:
		return "UNKNOWN";
	}
}

static void RecordResource(
	ID3D12Resource *resource, const D3D12_RESOURCE_DESC *desc,
	const D3D12_HEAP_PROPERTIES *heapProperties)
{
	if (!resource)
		return;

	ResourceRecord record;
	record.resource = resource;
	record.desc = desc ? *desc : resource->GetDesc();
	if (heapProperties) {
		record.heapType = heapProperties->Type;
		record.hasHeapType = true;
	}

	if (record.hasHeapType && record.heapType == D3D12_HEAP_TYPE_READBACK)
		return;

	record.resource->AddRef();
	if (record.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
		record.gpuVirtualAddress = resource->GetGPUVirtualAddress();
		record.size = record.desc.Width;
	}

	AcquireSRWLockExclusive(&gResourceLock);
	auto found = gResourceByPtr.find(resource);
	if (found != gResourceByPtr.end()) {
		if (gResources[found->second].resource)
			gResources[found->second].resource->Release();
		gResources[found->second] = record;
	} else {
		gResourceByPtr[resource] = gResources.size();
		gResources.push_back(record);
	}
	ReleaseSRWLockExclusive(&gResourceLock);
}

static void CleanupTrackedResources()
{
	AcquireSRWLockExclusive(&gResourceLock);
	for (ResourceRecord &record : gResources) {
		if (record.resource) {
			record.resource->Release();
			record.resource = nullptr;
		}
	}
	gResources.clear();
	gResourceByPtr.clear();
	ReleaseSRWLockExclusive(&gResourceLock);
}

static void FillResourceInfo(DescriptorRecord *record, ID3D12Resource *resource)
{
	if (!record || !resource)
		return;

	record->resource = resource;
	record->resourceDesc = resource->GetDesc();
	record->hasResourceDesc = true;
	if (record->resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		record->gpuVirtualAddress = resource->GetGPUVirtualAddress();

	AcquireSRWLockShared(&gResourceLock);
	auto found = gResourceByPtr.find(resource);
	if (found != gResourceByPtr.end() && found->second < gResources.size()) {
		const ResourceRecord &resourceRecord = gResources[found->second];
		if (resourceRecord.hasHeapType) {
			record->resourceHeapType = static_cast<UINT>(resourceRecord.heapType);
			record->hasResourceHeapType = true;
		}
	}
	ReleaseSRWLockShared(&gResourceLock);
}

static bool ResolveBufferByGpuVa(UINT64 gpuVa, UINT64 size, DescriptorRecord *record)
{
	if (!record || gpuVa == 0)
		return false;

	bool resolved = false;
	AcquireSRWLockShared(&gResourceLock);
	for (const ResourceRecord &resource : gResources) {
		if (resource.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
			resource.gpuVirtualAddress == 0 || resource.size == 0)
			continue;
		const UINT64 begin = resource.gpuVirtualAddress;
		const UINT64 end = begin + resource.size;
		const UINT64 requestedEnd = gpuVa + size;
		if (gpuVa >= begin && requestedEnd <= end) {
			record->resource = resource.resource;
			record->resourceDesc = resource.desc;
			record->hasResourceDesc = true;
			record->resourceOffset = gpuVa - begin;
			record->viewSize = size;
			if (resource.hasHeapType) {
				record->resourceHeapType = static_cast<UINT>(resource.heapType);
				record->hasResourceHeapType = true;
			}
			resolved = true;
			break;
		}
	}
	ReleaseSRWLockShared(&gResourceLock);
	return resolved;
}

static void RecordDescriptor(DescriptorRecord &&record)
{
	AcquireSRWLockExclusive(&gResourceLock);
	auto found = gDescriptorByCpuHandle.find(record.cpuHandle);
	if (found != gDescriptorByCpuHandle.end()) {
		gDescriptors[found->second] = std::move(record);
	} else {
		gDescriptorByCpuHandle[record.cpuHandle] = gDescriptors.size();
		gDescriptors.push_back(std::move(record));
	}
	ReleaseSRWLockExclusive(&gResourceLock);
}

static bool FindLatestDescriptorLocked(SIZE_T cpuHandle, DescriptorRecord *record)
{
	if (!record)
		return false;

	auto found = gDescriptorByCpuHandle.find(cpuHandle);
	if (found == gDescriptorByCpuHandle.end() || found->second >= gDescriptors.size())
		return false;

	*record = gDescriptors[found->second];
	return true;
}

static void RecordDescriptorLocked(DescriptorRecord &&record)
{
	auto found = gDescriptorByCpuHandle.find(record.cpuHandle);
	if (found != gDescriptorByCpuHandle.end()) {
		gDescriptors[found->second] = std::move(record);
	} else {
		gDescriptorByCpuHandle[record.cpuHandle] = gDescriptors.size();
		gDescriptors.push_back(std::move(record));
	}
}

static void RecordDescriptorCopyRange(
	ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
	D3D12_CPU_DESCRIPTOR_HANDLE destStart, D3D12_CPU_DESCRIPTOR_HANDLE srcStart,
	UINT count)
{
	if (!device || count == 0)
		return;

	const UINT increment = device->GetDescriptorHandleIncrementSize(type);
	if (increment == 0)
		return;

	AcquireSRWLockExclusive(&gResourceLock);
	for (UINT i = 0; i < count; ++i) {
		const SIZE_T srcHandle = srcStart.ptr + static_cast<SIZE_T>(i) * increment;
		const SIZE_T destHandle = destStart.ptr + static_cast<SIZE_T>(i) * increment;
		DescriptorRecord copied;
		if (FindLatestDescriptorLocked(srcHandle, &copied)) {
			copied.cpuHandle = destHandle;
			RecordDescriptorLocked(std::move(copied));
		}
	}
	ReleaseSRWLockExclusive(&gResourceLock);
}

static HRESULT STDMETHODCALLTYPE HookedCreateDescriptorHeap(
	ID3D12Device *device, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **heap)
{
	HRESULT hr = gOrigCreateDescriptorHeap(device, desc, riid, heap);
	if (SUCCEEDED(hr) && desc && heap && *heap) {
		ID3D12DescriptorHeap *descriptorHeap = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*heap)->QueryInterface(IID_PPV_ARGS(&descriptorHeap)))) {
			DescriptorHeapRecord record;
			record.heap = descriptorHeap;
			record.desc = *desc;
			record.cpuStart = descriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;
			record.gpuStart = descriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr;
			record.increment = device->GetDescriptorHandleIncrementSize(desc->Type);
			descriptorHeap->Release();

			AcquireSRWLockExclusive(&gResourceLock);
			gDescriptorHeaps.push_back(record);
			ReleaseSRWLockExclusive(&gResourceLock);
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateRootSignature(
	ID3D12Device *device, UINT nodeMask, const void *blob, SIZE_T blobLength,
	REFIID riid, void **rootSignature)
{
	HRESULT hr = gOrigCreateRootSignature(device, nodeMask, blob, blobLength, riid, rootSignature);
	if (SUCCEEDED(hr) && blob && blobLength && rootSignature && *rootSignature) {
		RootSignatureRecord record;
		record.rootSignature = static_cast<ID3D12RootSignature*>(*rootSignature);
		record.hash = Fnv1a64(blob, blobLength);
		record.size = blobLength;
		record.nodeMask = nodeMask;

		AcquireSRWLockExclusive(&gResourceLock);
		gRootSignatures.push_back(record);
		ReleaseSRWLockExclusive(&gResourceLock);
	}
	return hr;
}

static void STDMETHODCALLTYPE HookedCreateConstantBufferView(
	ID3D12Device *device, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
	D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateConstantBufferView(device, desc, destDescriptor);
	if (desc) {
		DescriptorRecord record;
		record.kind = "CBV";
		record.cpuHandle = destDescriptor.ptr;
		record.cbv = *desc;
		record.hasDesc = true;
		record.gpuVirtualAddress = desc->BufferLocation;
		record.viewSize = desc->SizeInBytes;
		ResolveBufferByGpuVa(desc->BufferLocation, desc->SizeInBytes, &record);
		RecordDescriptor(std::move(record));
	}
}

static void STDMETHODCALLTYPE HookedCreateShaderResourceView(
	ID3D12Device *device, ID3D12Resource *resource,
	const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateShaderResourceView(device, resource, desc, destDescriptor);
	DescriptorRecord record;
	record.kind = "SRV";
	record.cpuHandle = destDescriptor.ptr;
	FillResourceInfo(&record, resource);
	if (desc) {
		record.srv = *desc;
		record.hasDesc = true;
	}
	RecordDescriptor(std::move(record));
}

static void STDMETHODCALLTYPE HookedCreateUnorderedAccessView(
	ID3D12Device *device, ID3D12Resource *resource, ID3D12Resource *counterResource,
	const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateUnorderedAccessView(device, resource, counterResource, desc, destDescriptor);
	DescriptorRecord record;
	record.kind = "UAV";
	record.cpuHandle = destDescriptor.ptr;
	record.counterResource = counterResource;
	FillResourceInfo(&record, resource);
	if (desc) {
		record.uav = *desc;
		record.hasDesc = true;
	}
	RecordDescriptor(std::move(record));
}

static void STDMETHODCALLTYPE HookedCreateRenderTargetView(
	ID3D12Device *device, ID3D12Resource *resource,
	const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateRenderTargetView(device, resource, desc, destDescriptor);
	DescriptorRecord record;
	record.kind = "RTV";
	record.cpuHandle = destDescriptor.ptr;
	FillResourceInfo(&record, resource);
	if (desc) {
		record.rtv = *desc;
		record.hasDesc = true;
	}
	RecordDescriptor(std::move(record));
}

static void STDMETHODCALLTYPE HookedCreateDepthStencilView(
	ID3D12Device *device, ID3D12Resource *resource,
	const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateDepthStencilView(device, resource, desc, destDescriptor);
	DescriptorRecord record;
	record.kind = "DSV";
	record.cpuHandle = destDescriptor.ptr;
	FillResourceInfo(&record, resource);
	if (desc) {
		record.dsv = *desc;
		record.hasDesc = true;
	}
	RecordDescriptor(std::move(record));
}

static void STDMETHODCALLTYPE HookedCreateSampler(
	ID3D12Device *device, const D3D12_SAMPLER_DESC *desc,
	D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateSampler(device, desc, destDescriptor);
	if (desc) {
		DescriptorRecord record;
		record.kind = "Sampler";
		record.cpuHandle = destDescriptor.ptr;
		record.sampler = *desc;
		record.hasDesc = true;
		RecordDescriptor(std::move(record));
	}
}

static void STDMETHODCALLTYPE HookedCopyDescriptors(
	ID3D12Device *device, UINT numDestDescriptorRanges,
	const D3D12_CPU_DESCRIPTOR_HANDLE *destDescriptorRangeStarts,
	const UINT *destDescriptorRangeSizes, UINT numSrcDescriptorRanges,
	const D3D12_CPU_DESCRIPTOR_HANDLE *srcDescriptorRangeStarts,
	const UINT *srcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapsType)
{
	gOrigCopyDescriptors(device, numDestDescriptorRanges, destDescriptorRangeStarts,
		destDescriptorRangeSizes, numSrcDescriptorRanges, srcDescriptorRangeStarts,
		srcDescriptorRangeSizes, descriptorHeapsType);

	if (!destDescriptorRangeStarts || !srcDescriptorRangeStarts)
		return;

	UINT destRange = 0;
	UINT srcRange = 0;
	UINT destOffset = 0;
	UINT srcOffset = 0;
	while (destRange < numDestDescriptorRanges && srcRange < numSrcDescriptorRanges) {
		const UINT destRangeSize = destDescriptorRangeSizes ? destDescriptorRangeSizes[destRange] : 1;
		const UINT srcRangeSize = srcDescriptorRangeSizes ? srcDescriptorRangeSizes[srcRange] : 1;
		if (destOffset >= destRangeSize) {
			destRange++;
			destOffset = 0;
			continue;
		}
		if (srcOffset >= srcRangeSize) {
			srcRange++;
			srcOffset = 0;
			continue;
		}

		const UINT count = min(destRangeSize - destOffset, srcRangeSize - srcOffset);
		const UINT increment = device ? device->GetDescriptorHandleIncrementSize(descriptorHeapsType) : 0;
		if (increment) {
			D3D12_CPU_DESCRIPTOR_HANDLE dest = destDescriptorRangeStarts[destRange];
			D3D12_CPU_DESCRIPTOR_HANDLE src = srcDescriptorRangeStarts[srcRange];
			dest.ptr += static_cast<SIZE_T>(destOffset) * increment;
			src.ptr += static_cast<SIZE_T>(srcOffset) * increment;
			RecordDescriptorCopyRange(device, descriptorHeapsType, dest, src, count);
		}
		destOffset += count;
		srcOffset += count;
	}
}

static void STDMETHODCALLTYPE HookedCopyDescriptorsSimple(
	ID3D12Device *device, UINT numDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptorRangeStart,
	D3D12_CPU_DESCRIPTOR_HANDLE srcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapsType)
{
	gOrigCopyDescriptorsSimple(device, numDescriptors, destDescriptorRangeStart,
		srcDescriptorRangeStart, descriptorHeapsType);
	RecordDescriptorCopyRange(device, descriptorHeapsType, destDescriptorRangeStart,
		srcDescriptorRangeStart, numDescriptors);
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommittedResource(
	ID3D12Device *device, const D3D12_HEAP_PROPERTIES *heapProperties,
	D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC *desc,
	D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE *optimizedClearValue,
	REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateCommittedResource(device, heapProperties, heapFlags, desc,
		initialState, optimizedClearValue, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, heapProperties);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePlacedResource(
	ID3D12Device *device, ID3D12Heap *heap, UINT64 heapOffset,
	const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initialState,
	const D3D12_CLEAR_VALUE *optimizedClearValue, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreatePlacedResource(device, heap, heapOffset, desc,
		initialState, optimizedClearValue, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, nullptr);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateReservedResource(
	ID3D12Device *device, const D3D12_RESOURCE_DESC *desc,
	D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE *optimizedClearValue,
	REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateReservedResource(device, desc, initialState,
		optimizedClearValue, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, nullptr);
			d3dResource->Release();
		}
	}
	return hr;
}

void DX12HookResourceMetadata(ID3D12Device *device)
{
	if (!device)
		return;

	if (!gCleanupRegistered) {
		atexit(CleanupTrackedResources);
		gCleanupRegistered = true;
	}

	void **vtable = *reinterpret_cast<void***>(device);
	if (!vtable)
		return;

	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateDescriptorHeap),
		vtable[14], HookedCreateDescriptorHeap, "ID3D12Device::CreateDescriptorHeap");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateRootSignature),
		vtable[16], HookedCreateRootSignature, "ID3D12Device::CreateRootSignature");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateConstantBufferView),
		vtable[17], HookedCreateConstantBufferView, "ID3D12Device::CreateConstantBufferView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateShaderResourceView),
		vtable[18], HookedCreateShaderResourceView, "ID3D12Device::CreateShaderResourceView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateUnorderedAccessView),
		vtable[19], HookedCreateUnorderedAccessView, "ID3D12Device::CreateUnorderedAccessView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateRenderTargetView),
		vtable[20], HookedCreateRenderTargetView, "ID3D12Device::CreateRenderTargetView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateDepthStencilView),
		vtable[21], HookedCreateDepthStencilView, "ID3D12Device::CreateDepthStencilView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateSampler),
		vtable[22], HookedCreateSampler, "ID3D12Device::CreateSampler");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCopyDescriptors),
		vtable[23], HookedCopyDescriptors, "ID3D12Device::CopyDescriptors");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCopyDescriptorsSimple),
		vtable[24], HookedCopyDescriptorsSimple, "ID3D12Device::CopyDescriptorsSimple");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommittedResource),
		vtable[27], HookedCreateCommittedResource, "ID3D12Device::CreateCommittedResource");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreatePlacedResource),
		vtable[29], HookedCreatePlacedResource, "ID3D12Device::CreatePlacedResource");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateReservedResource),
		vtable[30], HookedCreateReservedResource, "ID3D12Device::CreateReservedResource");
}

void DX12RecordPsoRootSignature(
	UINT64 psoIndex, const char *kind, ID3D12PipelineState *pipelineState,
	ID3D12RootSignature *rootSignature)
{
	PsoRootRecord record;
	record.psoIndex = psoIndex;
	record.kind = kind ? kind : "";
	record.pipelineState = pipelineState;
	record.rootSignature = rootSignature;

	AcquireSRWLockExclusive(&gResourceLock);
	gPsoRoots.push_back(record);
	ReleaseSRWLockExclusive(&gResourceLock);
}

static void FillDescriptorSummary(DX12DescriptorSummary *summary, const DescriptorRecord &record)
{
	if (!summary)
		return;

	summary->kind = record.kind;
	summary->cpuHandle = record.cpuHandle;
	summary->resource = record.resource;
	summary->counterResource = record.counterResource;
	summary->resourceDesc = record.resourceDesc;
	summary->hasResourceDesc = record.hasResourceDesc;
	summary->resourceHeapType = record.resourceHeapType;
	summary->hasResourceHeapType = record.hasResourceHeapType;
	summary->gpuVirtualAddress = record.gpuVirtualAddress;
	summary->resourceOffset = record.resourceOffset;
	summary->viewSize = record.viewSize;
	summary->hasDesc = record.hasDesc;

	if (record.kind == "CBV") {
		summary->cbvSize = record.cbv.SizeInBytes;
	} else if (record.kind == "SRV" && record.hasDesc) {
		summary->viewFormat = static_cast<UINT>(record.srv.Format);
		summary->viewDimension = static_cast<UINT>(record.srv.ViewDimension);
	} else if (record.kind == "UAV" && record.hasDesc) {
		summary->viewFormat = static_cast<UINT>(record.uav.Format);
		summary->viewDimension = static_cast<UINT>(record.uav.ViewDimension);
	} else if (record.kind == "RTV" && record.hasDesc) {
		summary->viewFormat = static_cast<UINT>(record.rtv.Format);
		summary->viewDimension = static_cast<UINT>(record.rtv.ViewDimension);
	} else if (record.kind == "DSV" && record.hasDesc) {
		summary->viewFormat = static_cast<UINT>(record.dsv.Format);
		summary->viewDimension = static_cast<UINT>(record.dsv.ViewDimension);
	}
}

void DX12GetResourceMetadataSnapshot(
	std::vector<DX12RootSignatureSummary> *rootSignatures,
	std::vector<DX12DescriptorSummary> *descriptors,
	std::vector<DX12PsoRootSummary> *psoRoots)
{
	DX12GetResourceMetadataSnapshot(rootSignatures, descriptors, psoRoots, nullptr);
}

void DX12GetResourceMetadataSnapshot(
	std::vector<DX12RootSignatureSummary> *rootSignatures,
	std::vector<DX12DescriptorSummary> *descriptors,
	std::vector<DX12PsoRootSummary> *psoRoots,
	std::vector<DX12DescriptorHeapSummary> *descriptorHeaps)
{
	AcquireSRWLockShared(&gResourceLock);

	if (rootSignatures) {
		rootSignatures->clear();
		rootSignatures->reserve(gRootSignatures.size());
		for (const RootSignatureRecord &record : gRootSignatures) {
			DX12RootSignatureSummary summary;
			summary.rootSignature = record.rootSignature;
			summary.hash = record.hash;
			summary.size = record.size;
			summary.nodeMask = record.nodeMask;
			rootSignatures->push_back(summary);
		}
	}

	if (descriptors) {
		descriptors->clear();
		descriptors->reserve(gDescriptors.size());
		for (const DescriptorRecord &record : gDescriptors) {
			DX12DescriptorSummary summary;
			FillDescriptorSummary(&summary, record);
			descriptors->push_back(summary);
		}
	}

	if (psoRoots) {
		psoRoots->clear();
		psoRoots->reserve(gPsoRoots.size());
		for (const PsoRootRecord &record : gPsoRoots) {
			DX12PsoRootSummary summary;
			summary.psoIndex = record.psoIndex;
			summary.kind = record.kind;
			summary.pipelineState = record.pipelineState;
			summary.rootSignature = record.rootSignature;
			psoRoots->push_back(summary);
		}
	}

	if (descriptorHeaps) {
		descriptorHeaps->clear();
		descriptorHeaps->reserve(gDescriptorHeaps.size());
		for (const DescriptorHeapRecord &record : gDescriptorHeaps) {
			DX12DescriptorHeapSummary summary;
			summary.heap = record.heap;
			summary.type = static_cast<UINT>(record.desc.Type);
			summary.numDescriptors = record.desc.NumDescriptors;
			summary.flags = static_cast<UINT>(record.desc.Flags);
			summary.nodeMask = record.desc.NodeMask;
			summary.cpuStart = record.cpuStart;
			summary.gpuStart = record.gpuStart;
			summary.increment = record.increment;
			descriptorHeaps->push_back(summary);
		}
	}

	ReleaseSRWLockShared(&gResourceLock);
}

static void WriteResourceDesc(FILE *file, const DescriptorRecord &record)
{
	if (!record.hasResourceDesc) {
		fprintf(file, ",,,,,,,,,");
		return;
	}

	const D3D12_RESOURCE_DESC &desc = record.resourceDesc;
	fprintf(file, ",%s,%llu,%u,%u,%u,%u,0x%llx,0x%llx,%llu",
		ResourceDimensionName(desc.Dimension),
		static_cast<unsigned long long>(desc.Width),
		desc.Height,
		desc.DepthOrArraySize,
		desc.MipLevels,
		static_cast<UINT>(desc.Format),
		static_cast<unsigned long long>(desc.Flags),
		static_cast<unsigned long long>(record.gpuVirtualAddress),
		static_cast<unsigned long long>(desc.SampleDesc.Count));
}

void DX12DumpResourceMetadata(const wchar_t *dir)
{
	if (!dir)
		return;

	std::vector<RootSignatureRecord> rootSignatures;
	std::vector<DescriptorHeapRecord> descriptorHeaps;
	std::vector<DescriptorRecord> descriptors;
	std::vector<PsoRootRecord> psoRoots;

	AcquireSRWLockShared(&gResourceLock);
	rootSignatures = gRootSignatures;
	descriptorHeaps = gDescriptorHeaps;
	descriptors = gDescriptors;
	psoRoots = gPsoRoots;
	ReleaseSRWLockShared(&gResourceLock);

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\ResourceMetadataDX12.txt", dir);
	FILE *file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;

	fprintf(file, "DX12 Resource Metadata\n");
	fprintf(file, "======================\n");
	fprintf(file, "root_signatures=%zu descriptor_heaps=%zu descriptors=%zu pso_roots=%zu\n\n",
		rootSignatures.size(), descriptorHeaps.size(), descriptors.size(), psoRoots.size());

	fprintf(file, "PSO Root Signatures\n");
	fprintf(file, "pso,kind,pipeline_state,root_signature\n");
	for (const PsoRootRecord &record : psoRoots) {
		fprintf(file, "%llu,%s,%p,%p\n",
			static_cast<unsigned long long>(record.psoIndex),
			record.kind.c_str(), record.pipelineState, record.rootSignature);
	}

	fprintf(file, "\nRoot Signatures\n");
	fprintf(file, "index,root_signature,hash,size,node_mask\n");
	for (size_t i = 0; i < rootSignatures.size(); ++i) {
		const RootSignatureRecord &record = rootSignatures[i];
		fprintf(file, "%zu,%p,%016llx,%zu,%u\n",
			i,
			record.rootSignature,
			static_cast<unsigned long long>(record.hash),
			record.size,
			record.nodeMask);
	}

	fprintf(file, "\nDescriptor Heaps\n");
	fprintf(file, "index,heap,type,num_descriptors,flags,node_mask,cpu_start,gpu_start,increment\n");
	for (size_t i = 0; i < descriptorHeaps.size(); ++i) {
		const DescriptorHeapRecord &record = descriptorHeaps[i];
		fprintf(file, "%zu,%p,%s,%u,0x%x,%u,0x%llx,0x%llx,%u\n",
			i,
			record.heap,
			DescriptorHeapTypeName(record.desc.Type),
			record.desc.NumDescriptors,
			static_cast<UINT>(record.desc.Flags),
			record.desc.NodeMask,
			static_cast<unsigned long long>(record.cpuStart),
			static_cast<unsigned long long>(record.gpuStart),
			record.increment);
	}

	fprintf(file, "\nDescriptors\n");
	fprintf(file,
		"index,kind,cpu_handle,resource,counter_resource,resource_dimension,width,height,depth_or_array,mips,format,flags,gpu_va,sample_count,resource_offset,view_size,heap_type,has_view_desc,view_format,view_dimension,cbv_size\n");
	for (size_t i = 0; i < descriptors.size(); ++i) {
		const DescriptorRecord &record = descriptors[i];
		fprintf(file, "%zu,%s,0x%llx,%p,%p",
			i,
			record.kind.c_str(),
			static_cast<unsigned long long>(record.cpuHandle),
			record.resource,
			record.counterResource);
		WriteResourceDesc(file, record);

		UINT viewFormat = 0;
		UINT viewDimension = 0;
		UINT cbvSize = 0;
		if (record.kind == "CBV") {
			cbvSize = record.cbv.SizeInBytes;
		} else if (record.kind == "SRV" && record.hasDesc) {
			viewFormat = static_cast<UINT>(record.srv.Format);
			viewDimension = static_cast<UINT>(record.srv.ViewDimension);
		} else if (record.kind == "UAV" && record.hasDesc) {
			viewFormat = static_cast<UINT>(record.uav.Format);
			viewDimension = static_cast<UINT>(record.uav.ViewDimension);
		} else if (record.kind == "RTV" && record.hasDesc) {
			viewFormat = static_cast<UINT>(record.rtv.Format);
			viewDimension = static_cast<UINT>(record.rtv.ViewDimension);
		} else if (record.kind == "DSV" && record.hasDesc) {
			viewFormat = static_cast<UINT>(record.dsv.Format);
			viewDimension = static_cast<UINT>(record.dsv.ViewDimension);
		}

		fprintf(file, ",%llu,%llu,%u,%u,%u,%u,%u\n",
			static_cast<unsigned long long>(record.resourceOffset),
			static_cast<unsigned long long>(record.viewSize),
			record.hasResourceHeapType ? record.resourceHeapType : 0,
			record.hasDesc ? 1 : 0,
			viewFormat,
			viewDimension,
			cbvSize);
	}

	fclose(file);
	DX12Log("Resource metadata written: %S roots=%zu heaps=%zu descriptors=%zu psoRoots=%zu\n",
		path, rootSignatures.size(), descriptorHeaps.size(), descriptors.size(), psoRoots.size());
}
