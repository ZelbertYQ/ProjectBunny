#include "DX12ResourceTracker.h"

#include <Shlwapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
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

static PFN_CREATE_DESCRIPTOR_HEAP gOrigCreateDescriptorHeap = nullptr;
static PFN_CREATE_ROOT_SIGNATURE gOrigCreateRootSignature = nullptr;
static PFN_CREATE_CONSTANT_BUFFER_VIEW gOrigCreateConstantBufferView = nullptr;
static PFN_CREATE_SHADER_RESOURCE_VIEW gOrigCreateShaderResourceView = nullptr;
static PFN_CREATE_UNORDERED_ACCESS_VIEW gOrigCreateUnorderedAccessView = nullptr;
static PFN_CREATE_RENDER_TARGET_VIEW gOrigCreateRenderTargetView = nullptr;
static PFN_CREATE_DEPTH_STENCIL_VIEW gOrigCreateDepthStencilView = nullptr;
static PFN_CREATE_SAMPLER gOrigCreateSampler = nullptr;

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
	UINT64 gpuVirtualAddress = 0;
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
	D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
	D3D12_SAMPLER_DESC sampler = {};
	bool hasDesc = false;
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
static std::vector<PsoRootRecord> gPsoRoots;

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

static void FillResourceInfo(DescriptorRecord *record, ID3D12Resource *resource)
{
	if (!record || !resource)
		return;

	record->resource = resource;
	record->resourceDesc = resource->GetDesc();
	record->hasResourceDesc = true;
	if (record->resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		record->gpuVirtualAddress = resource->GetGPUVirtualAddress();
}

static void RecordDescriptor(DescriptorRecord &&record)
{
	AcquireSRWLockExclusive(&gResourceLock);
	gDescriptors.push_back(std::move(record));
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

void DX12HookResourceMetadata(ID3D12Device *device)
{
	if (!device)
		return;

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
		"index,kind,cpu_handle,resource,counter_resource,resource_dimension,width,height,depth_or_array,mips,format,flags,gpu_va,sample_count,has_view_desc,view_format,view_dimension,cbv_size\n");
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

		fprintf(file, ",%u,%u,%u,%u\n",
			record.hasDesc ? 1 : 0,
			viewFormat,
			viewDimension,
			cbvSize);
	}

	fclose(file);
	DX12Log("Resource metadata written: %S roots=%zu heaps=%zu descriptors=%zu psoRoots=%zu\n",
		path, rootSignatures.size(), descriptorHeaps.size(), descriptors.size(), psoRoots.size());
}
