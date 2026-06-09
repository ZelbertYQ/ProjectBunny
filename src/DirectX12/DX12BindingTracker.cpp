#include "DX12BindingTracker.h"

#include <stdio.h>

#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

#include "DX12ResourceTracker.h"
#include "DX12ShaderDump.h"
#include "DX12State.h"

static constexpr UINT MaxRootParameters = 64;
static constexpr UINT MaxTrackedEvents = 20000;

struct RootTableState
{
	bool valid = false;
	UINT rootParameterIndex = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor = {};
};

struct CommandListBindingState
{
	ID3D12PipelineState *pipelineState = nullptr;
	ID3D12DescriptorHeap *cbvSrvUavHeap = nullptr;
	ID3D12DescriptorHeap *samplerHeap = nullptr;
	RootTableState graphicsTables[MaxRootParameters] = {};
	RootTableState computeTables[MaxRootParameters] = {};
	UINT64 drawSerial = 0;
	UINT64 dispatchSerial = 0;
};

struct BindingEvent
{
	UINT64 serial = 0;
	std::string kind;
	ID3D12GraphicsCommandList *commandList = nullptr;
	ID3D12PipelineState *pipelineState = nullptr;
	DX12PsoShaderInfo shaderInfo = {};
	ID3D12DescriptorHeap *cbvSrvUavHeap = nullptr;
	ID3D12DescriptorHeap *samplerHeap = nullptr;
	RootTableState graphicsTables[MaxRootParameters] = {};
	RootTableState computeTables[MaxRootParameters] = {};
};

static SRWLOCK gBindingLock = SRWLOCK_INIT;
static std::unordered_map<ID3D12GraphicsCommandList*, CommandListBindingState> gCommandLists;
static std::vector<BindingEvent> gEvents;
static UINT64 gEventSerial = 0;
static UINT64 gDroppedEvents = 0;

static void StoreEventLocked(
	const char *kind, ID3D12GraphicsCommandList *commandList,
	const CommandListBindingState &state)
{
	if (gEvents.size() >= MaxTrackedEvents) {
		gDroppedEvents++;
		return;
	}

	BindingEvent event;
	event.serial = ++gEventSerial;
	event.kind = kind ? kind : "";
	event.commandList = commandList;
	event.pipelineState = state.pipelineState;
	event.cbvSrvUavHeap = state.cbvSrvUavHeap;
	event.samplerHeap = state.samplerHeap;
	memcpy(event.graphicsTables, state.graphicsTables, sizeof(event.graphicsTables));
	memcpy(event.computeTables, state.computeTables, sizeof(event.computeTables));
	if (state.pipelineState)
		DX12GetPipelineStateShaderInfo(state.pipelineState, &event.shaderInfo);
	gEvents.push_back(event);
}

void DX12BindingRegisterCommandList(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	gCommandLists.try_emplace(commandList);
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingResetCommandList(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *initialState)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state = CommandListBindingState();
	state.pipelineState = initialState;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	gCommandLists[commandList].pipelineState = pipelineState;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingRecordStateEvent(ID3D12GraphicsCommandList *commandList, const char *kind)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	StoreEventLocked(kind ? kind : "state", commandList, state);
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList, UINT count,
	ID3D12DescriptorHeap *const *heaps)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state.cbvSrvUavHeap = nullptr;
	state.samplerHeap = nullptr;
	for (UINT i = 0; i < count && heaps; ++i) {
		ID3D12DescriptorHeap *heap = heaps[i];
		if (!heap)
			continue;
		D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
		if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
			state.cbvSrvUavHeap = heap;
		else if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			state.samplerHeap = heap;
	}
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetGraphicsRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	if (!commandList || rootParameterIndex >= MaxRootParameters)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	RootTableState &table = gCommandLists[commandList].graphicsTables[rootParameterIndex];
	table.valid = true;
	table.rootParameterIndex = rootParameterIndex;
	table.baseDescriptor = baseDescriptor;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetComputeRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	if (!commandList || rootParameterIndex >= MaxRootParameters)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	RootTableState &table = gCommandLists[commandList].computeTables[rootParameterIndex];
	table.valid = true;
	table.rootParameterIndex = rootParameterIndex;
	table.baseDescriptor = baseDescriptor;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingRecordDraw(ID3D12GraphicsCommandList *commandList, const char *kind)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state.drawSerial++;
	StoreEventLocked(kind ? kind : "draw", commandList, state);
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingRecordDispatch(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state.dispatchSerial++;
	StoreEventLocked("dispatch", commandList, state);
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingBeginFrame()
{
	AcquireSRWLockExclusive(&gBindingLock);
	gEvents.clear();
	gEventSerial = 0;
	gDroppedEvents = 0;
	ReleaseSRWLockExclusive(&gBindingLock);
}

static void WriteShaderInfo(FILE *file, const DX12PsoShaderInfo &info)
{
	if (!file)
		return;

	fprintf(file, "%llu,",
		static_cast<unsigned long long>(info.psoIndex));
	if (info.hasVS)
		fprintf(file, "%016llx", static_cast<unsigned long long>(info.vs));
	fprintf(file, ",");
	if (info.hasPS)
		fprintf(file, "%016llx", static_cast<unsigned long long>(info.ps));
	fprintf(file, ",");
	if (info.hasCS)
		fprintf(file, "%016llx", static_cast<unsigned long long>(info.cs));
}

static void WriteRootTables(FILE *file, const RootTableState *tables)
{
	bool first = true;
	for (UINT i = 0; i < MaxRootParameters; ++i) {
		if (!tables[i].valid)
			continue;
		if (!first)
			fprintf(file, ";");
		fprintf(file, "%u=0x%llx",
			tables[i].rootParameterIndex,
			static_cast<unsigned long long>(tables[i].baseDescriptor.ptr));
		first = false;
	}
}

static const char *HeapTypeName(UINT type)
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

static const DX12DescriptorHeapSummary *FindHeapForGpuHandle(
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const RootTableState &table,
	UINT requiredType)
{
	for (const DX12DescriptorHeapSummary &heap : heaps) {
		if (heap.type != requiredType || heap.increment == 0 || heap.gpuStart == 0)
			continue;
		const UINT64 begin = heap.gpuStart;
		const UINT64 end = begin + static_cast<UINT64>(heap.numDescriptors) * heap.increment;
		if (table.baseDescriptor.ptr >= begin && table.baseDescriptor.ptr < end)
			return &heap;
	}
	return nullptr;
}

static const DX12DescriptorSummary *FindDescriptorByCpuHandle(
	const std::vector<DX12DescriptorSummary> &descriptors, SIZE_T cpuHandle)
{
	for (const DX12DescriptorSummary &descriptor : descriptors) {
		if (descriptor.cpuHandle == cpuHandle)
			return &descriptor;
	}
	return nullptr;
}

static void WriteFrameResourceBinding(
	FILE *file, const BindingEvent &event, const char *bindSpace,
	const RootTableState &table, UINT heapType,
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const std::vector<DX12DescriptorSummary> &descriptors,
	std::unordered_set<std::string> *seen)
{
	if (!file || !table.valid || !seen)
		return;

	const DX12DescriptorHeapSummary *heap = FindHeapForGpuHandle(heaps, table, heapType);
	if (!heap)
		return;

	const UINT64 descriptorIndex = (table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
	const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
	const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptors, cpuHandle);

	char key[256];
	sprintf_s(key, "%llu|%s|%u|%p|%llu",
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace,
		table.rootParameterIndex,
		heap->heap,
		static_cast<unsigned long long>(descriptorIndex));
	if (!seen->insert(key).second)
		return;

	fprintf(file, "%llu,%s,%u,%p,%s,%llu,0x%llx,0x%llx,0x%llx,",
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace,
		table.rootParameterIndex,
		heap->heap,
		HeapTypeName(heap->type),
		static_cast<unsigned long long>(descriptorIndex),
		static_cast<unsigned long long>(table.baseDescriptor.ptr),
		static_cast<unsigned long long>(cpuHandle),
		static_cast<unsigned long long>(heap->gpuStart));

	if (!descriptor) {
		fprintf(file, "untracked,0,0,0,UNKNOWN,0,0,0,0,0,0,0,0\n");
		return;
	}

	fprintf(file, "%s,%p,%p,%u,",
		descriptor->kind.c_str(),
		descriptor->resource,
		descriptor->counterResource,
		descriptor->hasDesc ? 1 : 0);

	if (descriptor->hasResourceDesc) {
		const D3D12_RESOURCE_DESC &desc = descriptor->resourceDesc;
		fprintf(file, "%s,%llu,%u,%u,%u,%u,0x%llx,0x%llx,%u\n",
			ResourceDimensionName(desc.Dimension),
			static_cast<unsigned long long>(desc.Width),
			desc.Height,
			desc.DepthOrArraySize,
			desc.MipLevels,
			static_cast<UINT>(desc.Format),
			static_cast<unsigned long long>(desc.Flags),
			static_cast<unsigned long long>(descriptor->gpuVirtualAddress),
			descriptor->viewDimension);
	} else {
		fprintf(file, "NONE,0,0,0,0,%u,0x0,0x%llx,%u\n",
			descriptor->viewFormat,
			static_cast<unsigned long long>(descriptor->gpuVirtualAddress),
			descriptor->viewDimension);
	}
}

static void CollectFrameResourceBinding(
	std::vector<DX12FrameResourceBinding> *bindings,
	const BindingEvent &event, const char *bindSpace,
	const RootTableState &table, UINT heapType,
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const std::vector<DX12DescriptorSummary> &descriptors,
	std::unordered_set<std::string> *seen)
{
	if (!bindings || !table.valid || !seen)
		return;

	const DX12DescriptorHeapSummary *heap = FindHeapForGpuHandle(heaps, table, heapType);
	if (!heap)
		return;

	const UINT64 descriptorIndex = (table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
	const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
	const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptors, cpuHandle);

	char key[256];
	sprintf_s(key, "%llu|%s|%u|%p|%llu",
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace,
		table.rootParameterIndex,
		heap->heap,
		static_cast<unsigned long long>(descriptorIndex));
	if (!seen->insert(key).second)
		return;

	DX12FrameResourceBinding binding;
	binding.psoIndex = event.shaderInfo.psoIndex;
	binding.bindSpace = bindSpace ? bindSpace : "";
	binding.rootParameterIndex = table.rootParameterIndex;
	binding.heap = heap->heap;
	binding.heapType = heap->type;
	binding.descriptorIndex = descriptorIndex;
	binding.gpuHandle = table.baseDescriptor.ptr;
	binding.cpuHandle = cpuHandle;
	binding.heapGpuStart = heap->gpuStart;
	if (descriptor) {
		binding.descriptor = *descriptor;
		binding.hasDescriptor = true;
	}
	bindings->push_back(std::move(binding));
}

void DX12GetCurrentFrameResourceBindings(std::vector<DX12FrameResourceBinding> *bindings)
{
	if (!bindings)
		return;

	std::vector<BindingEvent> events;
	AcquireSRWLockShared(&gBindingLock);
	events = gEvents;
	ReleaseSRWLockShared(&gBindingLock);

	std::vector<DX12DescriptorSummary> descriptors;
	std::vector<DX12DescriptorHeapSummary> heaps;
	DX12GetResourceMetadataSnapshot(nullptr, &descriptors, nullptr, &heaps);

	bindings->clear();
	std::unordered_set<std::string> seen;
	for (const BindingEvent &event : events) {
		for (UINT i = 0; i < MaxRootParameters; ++i) {
			CollectFrameResourceBinding(bindings, event, "graphics_cbv_srv_uav",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptors, &seen);
			CollectFrameResourceBinding(bindings, event, "graphics_sampler",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptors, &seen);
			CollectFrameResourceBinding(bindings, event, "compute_cbv_srv_uav",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptors, &seen);
			CollectFrameResourceBinding(bindings, event, "compute_sampler",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptors, &seen);
		}
	}
}

static void WriteFrameResourceFile(const wchar_t *dir, const std::vector<BindingEvent> &events)
{
	if (!dir)
		return;

	std::vector<DX12DescriptorSummary> descriptors;
	std::vector<DX12DescriptorHeapSummary> heaps;
	DX12GetResourceMetadataSnapshot(nullptr, &descriptors, nullptr, &heaps);

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\CurrentFrameResourcesDX12.txt", dir);
	FILE *file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;

	fprintf(file, "DX12 Current Frame Resources\n");
	fprintf(file, "============================\n");
	fprintf(file, "events=%zu descriptor_heaps=%zu descriptors=%zu\n\n",
		events.size(), heaps.size(), descriptors.size());
	fprintf(file,
		"pso,bind_space,root_param,heap,heap_type,descriptor_index,gpu_handle,cpu_handle,heap_gpu_start,descriptor_kind,resource,counter_resource,has_view_desc,resource_dimension,width,height,depth_or_array,mips,format,flags,gpu_va,view_dimension\n");

	std::unordered_set<std::string> seen;
	for (const BindingEvent &event : events) {
		for (UINT i = 0; i < MaxRootParameters; ++i) {
			WriteFrameResourceBinding(file, event, "graphics_cbv_srv_uav",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptors, &seen);
			WriteFrameResourceBinding(file, event, "graphics_sampler",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptors, &seen);
			WriteFrameResourceBinding(file, event, "compute_cbv_srv_uav",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptors, &seen);
			WriteFrameResourceBinding(file, event, "compute_sampler",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptors, &seen);
		}
	}

	fclose(file);
	DX12Log("Current-frame resources written: %S bindings=%zu heaps=%zu descriptors=%zu\n",
		path, seen.size(), heaps.size(), descriptors.size());
}

void DX12DumpBindingTrace(const wchar_t *dir)
{
	if (!dir)
		return;

	std::vector<BindingEvent> events;
	UINT64 droppedEvents = 0;
	AcquireSRWLockShared(&gBindingLock);
	events = gEvents;
	droppedEvents = gDroppedEvents;
	ReleaseSRWLockShared(&gBindingLock);

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\BindingTraceDX12.txt", dir);
	FILE *file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;

	fprintf(file, "DX12 Current Frame Binding Trace\n");
	fprintf(file, "================================\n");
	fprintf(file, "events=%zu dropped=%llu max_events=%u\n\n",
		events.size(),
		static_cast<unsigned long long>(droppedEvents),
		MaxTrackedEvents);

	fprintf(file,
		"serial,kind,command_list,pipeline_state,pso,vs,ps,cs,cbv_srv_uav_heap,sampler_heap,graphics_tables,compute_tables\n");
	for (const BindingEvent &event : events) {
		fprintf(file, "%llu,%s,%p,%p,",
			static_cast<unsigned long long>(event.serial),
			event.kind.c_str(),
			event.commandList,
			event.pipelineState);
		WriteShaderInfo(file, event.shaderInfo);
		fprintf(file, ",%p,%p,",
			event.cbvSrvUavHeap,
			event.samplerHeap);
		WriteRootTables(file, event.graphicsTables);
		fprintf(file, ",");
		WriteRootTables(file, event.computeTables);
		fprintf(file, "\n");
	}

	fclose(file);
	DX12Log("Current-frame binding trace written: %S events=%zu dropped=%llu\n",
		path, events.size(), static_cast<unsigned long long>(droppedEvents));
	WriteFrameResourceFile(dir, events);
}
