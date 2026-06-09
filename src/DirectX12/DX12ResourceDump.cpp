#include "DX12ResourceDump.h"

#include <Shlwapi.h>
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "DX12BindingTracker.h"
#include "DX12State.h"

static constexpr UINT64 MaxResourceDumpBytes = 256ull * 1024ull * 1024ull;

struct DumpTask
{
	DX12FrameResourceBinding binding;
	ID3D12Resource *source = nullptr;
	ID3D12Resource *readback = nullptr;
	D3D12_RESOURCE_DESC desc = {};
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT textureLayout = {};
	UINT textureRows = 0;
	UINT64 sourceOffset = 0;
	UINT64 copyBytes = 0;
	UINT64 readbackOffset = 0;
	D3D12_RESOURCE_STATES sourceState = D3D12_RESOURCE_STATE_COMMON;
	bool needsBarrier = false;
	bool isTexture = false;
	bool ready = false;
	bool copied = false;
	bool stateKnown = false;
	std::wstring fileName;
	const char *skipNote = nullptr;
};

static constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
	return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
		(static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
		(static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
		(static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
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

static bool EnsureDirectory(const wchar_t *path)
{
	return CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool UnsafeResourceCopyEnabled()
{
	wchar_t value[16] = {};
	DWORD chars = GetEnvironmentVariableW(L"MIGOTO_DX12_UNSAFE_RESOURCE_COPY", value, ARRAYSIZE(value));
	return chars > 0 && value[0] == L'1';
}

static UINT64 AlignUp(UINT64 value, UINT64 alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static D3D12_RESOURCE_STATES GuessSourceState(const DX12FrameResourceBinding &binding)
{
	if (binding.descriptor.kind == "CBV")
		return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	if (binding.descriptor.kind == "UAV")
		return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	if (binding.descriptor.kind == "RTV")
		return D3D12_RESOURCE_STATE_RENDER_TARGET;
	if (binding.descriptor.kind == "DSV")
		return D3D12_RESOURCE_STATE_DEPTH_WRITE;
	return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

static bool ResourceNeedsBarrier(const DX12DescriptorSummary &descriptor)
{
	if (descriptor.hasResourceHeapType &&
		(descriptor.resourceHeapType == D3D12_HEAP_TYPE_UPLOAD ||
			descriptor.resourceHeapType == D3D12_HEAP_TYPE_READBACK))
		return false;
	return true;
}

static bool StateTrackingAllowsCopy(const DX12DescriptorSummary &descriptor)
{
	if (descriptor.hasResourceHeapType &&
		(descriptor.resourceHeapType == D3D12_HEAP_TYPE_UPLOAD ||
			descriptor.resourceHeapType == D3D12_HEAP_TYPE_READBACK))
		return true;
	return descriptor.hasCurrentState;
}

static bool IsUploadHeapResource(const DX12DescriptorSummary &descriptor)
{
	return descriptor.hasResourceHeapType &&
		descriptor.resourceHeapType == D3D12_HEAP_TYPE_UPLOAD;
}

static void WriteResourceBarrier(
	ID3D12GraphicsCommandList *commandList, ID3D12Resource *resource,
	D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
	if (!commandList || !resource || beforeState == afterState)
		return;

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = beforeState;
	barrier.Transition.StateAfter = afterState;
	commandList->ResourceBarrier(1, &barrier);
}

static bool WaitForFence(ID3D12CommandQueue *queue, ID3D12Device *device)
{
	ID3D12Fence *fence = nullptr;
	HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	if (FAILED(hr) || !fence)
		return false;

	HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!eventHandle) {
		fence->Release();
		return false;
	}

	hr = queue->Signal(fence, 1);
	if (SUCCEEDED(hr) && fence->GetCompletedValue() < 1) {
		hr = fence->SetEventOnCompletion(1, eventHandle);
		if (SUCCEEDED(hr))
			WaitForSingleObject(eventHandle, 5000);
	}

	CloseHandle(eventHandle);
	fence->Release();
	return SUCCEEDED(hr);
}

static void WriteU32(FILE *file, uint32_t value)
{
	fwrite(&value, sizeof(value), 1, file);
}

static void WriteDDSHeader(FILE *file, const D3D12_RESOURCE_DESC &desc, UINT64 rowPitch)
{
	WriteU32(file, MakeFourCC('D', 'D', 'S', ' '));
	WriteU32(file, 124);
	WriteU32(file, 0x0002100f);
	WriteU32(file, desc.Height);
	WriteU32(file, static_cast<uint32_t>(desc.Width));
	WriteU32(file, static_cast<uint32_t>(rowPitch));
	WriteU32(file, 0);
	WriteU32(file, 1);
	for (int i = 0; i < 11; ++i)
		WriteU32(file, 0);

	WriteU32(file, 32);
	WriteU32(file, 0x00000004);
	WriteU32(file, MakeFourCC('D', 'X', '1', '0'));
	WriteU32(file, 0);
	WriteU32(file, 0);
	WriteU32(file, 0);
	WriteU32(file, 0);
	WriteU32(file, 0);

	WriteU32(file, 0x00001000);
	WriteU32(file, 0);
	WriteU32(file, 0);
	WriteU32(file, 0);
	WriteU32(file, 0);

	WriteU32(file, static_cast<uint32_t>(desc.Format));
	WriteU32(file, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
	WriteU32(file, 0);
	WriteU32(file, 1);
	WriteU32(file, 0);
}

static bool WriteTextureDDS(const wchar_t *path, const DumpTask &task, const void *mappedData)
{
	FILE *file = _wfsopen(path, L"wb", _SH_DENYNO);
	if (!file)
		return false;

	WriteDDSHeader(file, task.desc, task.textureLayout.Footprint.RowPitch);
	const uint8_t *base = static_cast<const uint8_t*>(mappedData) + task.readbackOffset;
	for (UINT row = 0; row < task.textureRows; ++row) {
		const uint8_t *src = base + static_cast<size_t>(row) * task.textureLayout.Footprint.RowPitch;
		fwrite(src, 1, task.textureLayout.Footprint.RowPitch, file);
	}
	fclose(file);
	return true;
}

static bool WriteBufferFile(const wchar_t *path, const DumpTask &task, const void *mappedData)
{
	FILE *file = _wfsopen(path, L"wb", _SH_DENYNO);
	if (!file)
		return false;

	const uint8_t *src = static_cast<const uint8_t*>(mappedData) + task.readbackOffset;
	fwrite(src, 1, static_cast<size_t>(task.copyBytes), file);
	fclose(file);
	return true;
}

static bool ShouldDumpBinding(const DX12FrameResourceBinding &binding)
{
	if (!binding.hasDescriptor || !binding.descriptor.resource || !binding.descriptor.hasResourceDesc)
		return false;
	if (binding.descriptor.kind != "SRV" &&
		binding.descriptor.kind != "UAV" &&
		binding.descriptor.kind != "CBV")
		return false;
	D3D12_RESOURCE_DIMENSION dimension = binding.descriptor.resourceDesc.Dimension;
	return dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
		dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
}

static bool PrepareTask(ID3D12Device *device, const DX12FrameResourceBinding &binding, DumpTask *task)
{
	if (!device || !task || !ShouldDumpBinding(binding))
		return false;

	const bool unsafeCopy = UnsafeResourceCopyEnabled();
	task->binding = binding;
	task->source = binding.descriptor.resource;
	task->desc = binding.descriptor.resourceDesc;
	task->sourceState = binding.descriptor.hasCurrentState ?
		static_cast<D3D12_RESOURCE_STATES>(binding.descriptor.currentState) :
		GuessSourceState(binding);
	task->stateKnown = StateTrackingAllowsCopy(binding.descriptor);
	if (!unsafeCopy && !task->stateKnown) {
		task->skipNote = "state_unknown";
		return false;
	}
	task->needsBarrier = ResourceNeedsBarrier(binding.descriptor);

	if (task->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
		if (task->desc.SampleDesc.Count > 1) {
			task->skipNote = "multisampled_texture_not_supported";
			return false;
		}
		UINT64 rowSize = 0;
		device->GetCopyableFootprints(&task->desc, 0, 1, 0,
			&task->textureLayout, &task->textureRows, &rowSize, &task->copyBytes);
		task->isTexture = true;
	} else if (task->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
		task->sourceOffset = binding.descriptor.resourceOffset;
		task->copyBytes = binding.descriptor.viewSize ? binding.descriptor.viewSize : task->desc.Width;
		if (task->sourceOffset + task->copyBytes > task->desc.Width)
			task->copyBytes = task->desc.Width > task->sourceOffset ? task->desc.Width - task->sourceOffset : 0;
		task->isTexture = false;
	} else {
		task->skipNote = "unsupported_dimension";
		return false;
	}

	if (task->copyBytes == 0 || task->copyBytes > MaxResourceDumpBytes) {
		task->skipNote = "copy_size_zero_or_too_large";
		return false;
	}

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC readbackDesc = {};
	readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	readbackDesc.Width = task->copyBytes;
	readbackDesc.Height = 1;
	readbackDesc.DepthOrArraySize = 1;
	readbackDesc.MipLevels = 1;
	readbackDesc.SampleDesc.Count = 1;
	readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	HRESULT hr = device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &readbackDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&task->readback));
	if (FAILED(hr) || !task->readback) {
		task->skipNote = "readback_create_failed";
		return false;
	}

	task->ready = true;
	return true;
}

static void ReleaseTasks(std::vector<DumpTask> *tasks)
{
	if (!tasks)
		return;
	for (DumpTask &task : *tasks) {
		if (task.readback) {
			task.readback->Release();
			task.readback = nullptr;
		}
	}
}

static void BuildFileName(const DumpTask &task, wchar_t *fileName, size_t fileNameCount)
{
	const wchar_t *ext = task.isTexture ? L"dds" : L"buf";
	swprintf_s(fileName, fileNameCount, L"pso%llu_r%u_%p.%s",
		static_cast<unsigned long long>(task.binding.psoIndex),
		task.binding.rootParameterIndex,
		task.binding.descriptor.resource,
		ext);
}

static bool ExecuteCopyBatch(
	ID3D12Device *device, ID3D12CommandQueue *queue, std::vector<DumpTask> *tasks)
{
	if (!device || !queue || !tasks || tasks->empty())
		return false;

	ID3D12CommandAllocator *allocator = nullptr;
	ID3D12GraphicsCommandList *commandList = nullptr;
	HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
	if (SUCCEEDED(hr))
		hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&commandList));
	if (FAILED(hr) || !allocator || !commandList) {
		if (commandList)
			commandList->Release();
		if (allocator)
			allocator->Release();
		return false;
	}

	for (DumpTask &task : *tasks) {
		if (!task.ready || !task.readback)
			continue;
		if (task.needsBarrier)
			WriteResourceBarrier(commandList, task.source, task.sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);

		if (task.isTexture) {
			D3D12_TEXTURE_COPY_LOCATION src = {};
			src.pResource = task.source;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION dst = {};
			dst.pResource = task.readback;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint = task.textureLayout;
			dst.PlacedFootprint.Offset = 0;
			commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		} else {
			commandList->CopyBufferRegion(task.readback, 0, task.source,
				task.sourceOffset, task.copyBytes);
		}

		if (task.needsBarrier)
			WriteResourceBarrier(commandList, task.source, D3D12_RESOURCE_STATE_COPY_SOURCE, task.sourceState);
		task.copied = true;
	}

	hr = commandList->Close();
	if (FAILED(hr)) {
		DX12Log("Current-frame resource copy batch close failed hr=0x%lx tasks=%zu\n",
			hr, tasks->size());
		commandList->Release();
		allocator->Release();
		return false;
	}
	ID3D12CommandList *lists[] = { commandList };
	DX12Log("Current-frame resource copy batch executing tasks=%zu\n", tasks->size());
	queue->ExecuteCommandLists(1, lists);
	bool ok = WaitForFence(queue, device);

	commandList->Release();
	allocator->Release();
	return ok;
}

void DX12DumpCurrentFrameResources(const wchar_t *dir)
{
	if (!dir)
		return;

	std::vector<DX12FrameResourceBinding> bindings;
	DX12GetCurrentFrameResourceBindings(&bindings);

	wchar_t resourceDir[MAX_PATH];
	swprintf_s(resourceDir, L"%s\\CurrentFrameResourceFiles", dir);
	EnsureDirectory(resourceDir);

	wchar_t indexPath[MAX_PATH];
	swprintf_s(indexPath, L"%s\\CurrentFrameResourceFilesDX12.txt", dir);
	FILE *index = _wfsopen(indexPath, L"w", _SH_DENYNO);
	if (!index)
		return;

	fprintf(index, "DX12 Current Frame Resource Files\n");
	fprintf(index, "=================================\n");
	fprintf(index, "bindings=%zu max_resource_bytes=%llu buffer_extension=.buf unsafe_resource_copy=%u state_tracking=1\n\n",
		bindings.size(), static_cast<unsigned long long>(MaxResourceDumpBytes),
		UnsafeResourceCopyEnabled() ? 1 : 0);
	fprintf(index,
		"status,pso,bind_space,root_param,descriptor_kind,resource,dimension,width,height,format,gpu_va,resource_offset,copy_bytes,current_state,has_current_state,file,note\n");

	ID3D12CommandQueue *queue = DX12AcquireCommandQueue();
	ID3D12Device *device = nullptr;
	if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) {
		fprintf(index, "error,0,,,,,,,,,,,,no_command_queue,missing command queue\n");
		if (queue)
			queue->Release();
		fclose(index);
		DX12Log("Current-frame resource file dump skipped: no command queue\n");
		return;
	}

	std::vector<DumpTask> tasks;
	std::vector<DumpTask> skippedTasks;
	std::unordered_set<std::string> dumpedKeys;
	UINT duplicates = 0;

	for (const DX12FrameResourceBinding &binding : bindings) {
		if (!binding.hasDescriptor) {
			DumpTask skipped;
			skipped.binding = binding;
			skipped.skipNote = "untracked_descriptor";
			skippedTasks.push_back(std::move(skipped));
			continue;
		}

		char key[128];
		sprintf_s(key, "%p|%llu|%llu",
			binding.descriptor.resource,
			static_cast<unsigned long long>(binding.descriptor.resourceOffset),
			static_cast<unsigned long long>(binding.descriptor.viewSize));
		if (binding.descriptor.resource && !dumpedKeys.insert(key).second) {
			DumpTask duplicate;
			duplicate.binding = binding;
			duplicate.skipNote = "already_dumped";
			skippedTasks.push_back(std::move(duplicate));
			duplicates++;
			continue;
		}

		DumpTask task;
		if (PrepareTask(device, binding, &task)) {
			wchar_t fileName[MAX_PATH];
			BuildFileName(task, fileName, ARRAYSIZE(fileName));
			task.fileName = fileName;
			tasks.push_back(std::move(task));
		} else {
			if (task.skipNote == nullptr)
				task.skipNote = "unsupported_or_no_resource_pointer";
			skippedTasks.push_back(std::move(task));
		}
	}

	const bool batchOk = ExecuteCopyBatch(device, queue, &tasks);

	UINT dumpedTextures = 0;
	UINT dumpedBuffers = 0;
	UINT failed = 0;
	for (DumpTask &task : tasks) {
		bool ok = batchOk && task.copied;
		if (ok) {
			void *mapped = nullptr;
			D3D12_RANGE range = { 0, static_cast<SIZE_T>(task.copyBytes) };
			if (SUCCEEDED(task.readback->Map(0, &range, &mapped)) && mapped) {
				wchar_t path[MAX_PATH];
				swprintf_s(path, L"%s\\%s", resourceDir, task.fileName.c_str());
				ok = task.isTexture ? WriteTextureDDS(path, task, mapped) :
					WriteBufferFile(path, task, mapped);
				D3D12_RANGE emptyRange = { 0, 0 };
				task.readback->Unmap(0, &emptyRange);
			} else {
				ok = false;
			}
		}

		if (ok && task.isTexture)
			dumpedTextures++;
		else if (ok)
			dumpedBuffers++;
		else
			failed++;

		fprintf(index, "%s,%llu,%s,%u,%s,%p,%s,%llu,%u,%u,0x%llx,%llu,%llu,0x%x,%u,%S,%s\n",
			ok ? "dumped" : "failed",
			static_cast<unsigned long long>(task.binding.psoIndex),
			task.binding.bindSpace.c_str(),
			task.binding.rootParameterIndex,
			task.binding.descriptor.kind.c_str(),
			task.binding.descriptor.resource,
			ResourceDimensionName(task.desc.Dimension),
			static_cast<unsigned long long>(task.desc.Width),
			task.desc.Height,
			static_cast<UINT>(task.desc.Format),
			static_cast<unsigned long long>(task.binding.descriptor.gpuVirtualAddress),
			static_cast<unsigned long long>(task.sourceOffset),
			static_cast<unsigned long long>(task.copyBytes),
			static_cast<UINT>(task.sourceState),
			task.stateKnown ? 1 : 0,
			ok ? task.fileName.c_str() : L"",
			ok ? "" : "copy_failed_or_write_failed");
	}

	UINT skipped = 0;
	for (const DumpTask &task : skippedTasks) {
		const bool duplicate = task.skipNote && !strcmp(task.skipNote, "already_dumped");
		if (!duplicate)
			skipped++;
		const DX12DescriptorSummary &descriptor = task.binding.descriptor;
		const D3D12_RESOURCE_DESC &desc = descriptor.resourceDesc;
		const char *dimensionName = descriptor.hasResourceDesc ?
			ResourceDimensionName(desc.Dimension) : "NONE";
		fprintf(index, "%s,%llu,%s,%u,%s,%p,%s,%llu,%u,%u,0x%llx,%llu,%llu,0x%x,%u,,%s\n",
			duplicate ? "duplicate" : "skipped",
			static_cast<unsigned long long>(task.binding.psoIndex),
			task.binding.bindSpace.c_str(),
			task.binding.rootParameterIndex,
			descriptor.kind.c_str(),
			descriptor.resource,
			dimensionName,
			descriptor.hasResourceDesc ? static_cast<unsigned long long>(desc.Width) : 0,
			descriptor.hasResourceDesc ? desc.Height : 0,
			descriptor.hasResourceDesc ? static_cast<UINT>(desc.Format) : 0,
			static_cast<unsigned long long>(descriptor.gpuVirtualAddress),
			static_cast<unsigned long long>(descriptor.resourceOffset),
			static_cast<unsigned long long>(descriptor.viewSize),
			descriptor.hasCurrentState ? descriptor.currentState : 0,
			descriptor.hasCurrentState ? 1 : 0,
			task.skipNote ? task.skipNote : "unsupported_or_no_resource_pointer");
	}

	ReleaseTasks(&tasks);
	device->Release();
	queue->Release();
	fclose(index);

	DX12Log("Current-frame resource files written: %S textures=%u buffers=%u skipped=%u duplicates=%u failed=%u bindings=%zu\n",
		indexPath, dumpedTextures, dumpedBuffers, skipped, duplicates, failed, bindings.size());
}
