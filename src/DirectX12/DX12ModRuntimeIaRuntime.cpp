static void ReleaseLoadedResourcesLocked()
{
	for (auto &item : gLoadedResources) {
		DX12RetiredLoadedResource retired;
		retired.resource = item.second;
		retired.retirePresent = DX12GetPresentCount();
		gRetiredLoadedResources.push_back(retired);
		item.second = DX12LoadedResource();
	}
	gLoadedResources.clear();
}

static void ReleaseLoadedResourceObjects(DX12LoadedResource *resource)
{
	if (!resource)
		return;
	if (resource->resource) {
		resource->resource->Release();
		resource->resource = nullptr;
	}
	if (resource->uavResource) {
		RetirePreSkinResource(resource->uavResource);
		resource->uavResource = nullptr;
	}
	if (resource->uavHeap) {
		RetirePreSkinDescriptorHeap(resource->uavHeap);
		resource->uavHeap = nullptr;
	}
	if (resource->srvResource) {
		resource->srvResource->Release();
		resource->srvResource = nullptr;
	}
	if (resource->srvHeap) {
		resource->srvHeap->Release();
		resource->srvHeap = nullptr;
	}
}

static void ReleaseExpiredRetiredLoadedResourcesLocked()
{
	const LONG present = DX12GetPresentCount();
	auto it = gRetiredLoadedResources.begin();
	while (it != gRetiredLoadedResources.end()) {
		if (present - it->retirePresent < RetiredLoadedResourcePresentDelay) {
			++it;
			continue;
		}
		ReleaseLoadedResourceObjects(&it->resource);
		it = gRetiredLoadedResources.erase(it);
	}
}

static bool CommandListRuntimeEffectLocked(const std::wstring &name);
static bool RunNamedCommandListLocked(
	ID3D12GraphicsCommandList *commandList,
	const std::wstring &name,
	DX12ModIaReplacement *replacement);

static bool TextureOverrideMatchCsSatisfiedLocked(const Bunny::TextureOverrideConfig &config)
{
	if (!config.hasMatchCs)
		return true;

	AcquireSRWLockShared(&gPreSkinLock);
	const bool active =
		gActivePreSkinTextureOverrides.find(config.section) != gActivePreSkinTextureOverrides.end();
	ReleaseSRWLockShared(&gPreSkinLock);
	return active;
}

void DX12ModBeginFrame()
{
	AcquireSRWLockExclusive(&gModLock);
	for (auto &item : gLoadedResources) {
		item.second.uavWritten = false;
		item.second.uavValid = false;
	}
	gPreSkinSectionAppliedPresent.clear();
	AcquireSRWLockExclusive(&gPreSkinLock);
	ClearActivePreSkinTextureOverridesLocked();
	ReleaseSRWLockExclusive(&gPreSkinLock);
	gPreSkinCbvReadCache.clear();
	gPreSkinSrvNegativeCache.clear();
	gPreSkinSrvPositiveCache.clear();
	ReleaseExpiredRetiredLoadedResourcesLocked();
	ReleaseSRWLockExclusive(&gModLock);
}

bool DX12ModPreparePresentReplacement(
	ID3D12GraphicsCommandList *commandList, DX12ModIaReplacement *replacement)
{
	if (!commandList || !replacement)
		return false;
	if (!DX12ModNeedsPresentReplacement())
		return false;

	const LONG present = DX12GetPresentCount();
	if (gPresentCommandListExecutedPresent == present)
		return false;
	if (InterlockedCompareExchange(
		    &gPresentCommandListExecutedPresent, present, present) == present)
		return false;

	AcquireSRWLockExclusive(&gModLock);
	if (InterlockedCompareExchange(
		    &gPresentCommandListExecutedPresent, present, present) == present) {
		ReleaseSRWLockExclusive(&gModLock);
		return false;
	}
	InterlockedExchange(&gPresentCommandListExecutedPresent, present);
	const bool changed = RunNamedCommandListLocked(commandList, L"Present", replacement);
	ReleaseSRWLockExclusive(&gModLock);
	return changed;
}

static const Bunny::TextureOverrideConfig *FindTextureOverrideLocked(
	uint32_t hash, uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex, uint32_t firstInstance, bool requireSkip,
	bool indexBuffer, uint32_t vertexSlot)
{
	const std::vector<size_t> *candidateIndexes =
		FindIaTextureOverrideCandidatesLocked(hash, indexBuffer, vertexSlot);
	if (!candidateIndexes || candidateIndexes->empty())
		return nullptr;

	for (size_t index : *candidateIndexes) {
		if (index >= gIaTextureOverrides.size())
			continue;
		const DX12IaTextureOverrideCandidate &candidate = gIaTextureOverrides[index];
		const Bunny::TextureOverrideConfig &config = candidate.config;
		if (!TextureOverrideMatchCsSatisfiedLocked(config))
			continue;
		if (!TextureOverrideMatchesIaBinding(config, indexBuffer, vertexSlot))
			continue;
		if (requireSkip && !config.handlingSkip)
			continue;
		if (!TextureOverrideMatchesDrawContext(
			config, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance))
			continue;
		return &config;
	}
	return nullptr;
}

static const DX12IaBufferHash *FindIaVertexSlot(const DX12IaHashState &iaState, uint32_t slot)
{
	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		if (buffer.slot == slot)
			return &buffer;
	}
	return nullptr;
}

static bool TargetStageIsCompute(Bunny::CommandListShaderStage stage)
{
	return stage == Bunny::CommandListShaderStage::Compute;
}

static D3D12_DESCRIPTOR_RANGE_TYPE DescriptorRangeTypeForTarget(
	Bunny::CommandListTargetKind kind)
{
	switch (kind) {
	case Bunny::CommandListTargetKind::ConstantBuffer:
		return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	case Bunny::CommandListTargetKind::ShaderResource:
		return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	case Bunny::CommandListTargetKind::UnorderedAccessView:
		return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	default:
		return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	}
}

static bool DescriptorBindingMatchesTarget(
	const DX12CurrentShaderResourceBinding &binding,
	const Bunny::CommandListTarget &target)
{
	if (binding.shaderRegister != target.slot || binding.registerSpace != 0)
		return false;
	if (binding.shaderVisibility == D3D12_SHADER_VISIBILITY_ALL)
		return true;
	if (target.stage == Bunny::CommandListShaderStage::Vertex)
		return binding.shaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX;
	if (target.stage == Bunny::CommandListShaderStage::Pixel)
		return binding.shaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL;
	if (target.stage == Bunny::CommandListShaderStage::Compute)
		return true;
	return false;
}

static uint32_t HashDescriptorBinding(const DX12CurrentShaderResourceBinding &binding)
{
	if (binding.hasDescriptor)
		return DX12HashDescriptorBufferView(
			&binding.descriptor, binding.gpuVirtualAddress,
			binding.descriptor.viewSize);
	if (binding.rootDescriptor && binding.gpuVirtualAddress) {
		DX12BufferResourceSummary summary = {};
		const UINT64 probeSize = 1;
		if (DX12ResolveBufferResourceByGpuVa(binding.gpuVirtualAddress, probeSize, &summary)) {
			const UINT64 size = summary.hasResourceDesc ?
				summary.resourceDesc.Width - summary.resourceOffset : summary.viewSize;
			return DX12HashBufferResourceView(
				&summary, binding.gpuVirtualAddress, size ? size : probeSize);
		}
		return DX12HashBufferResourceView(nullptr, binding.gpuVirtualAddress, probeSize);
	}
	return 0;
}

static const Bunny::TextureOverrideConfig *FindTextureOverrideByHashLocked(
	uint32_t hash, uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex, uint32_t firstInstance)
{
	if (!hash)
		return nullptr;

	auto bucket = gTextureOverrides.find(hash);
	if (bucket == gTextureOverrides.end())
		return nullptr;

	for (const Bunny::TextureOverrideConfig &config : bucket->second) {
		if (!TextureOverrideMatchCsSatisfiedLocked(config))
			continue;
		if (!TextureOverrideMatchesDrawContext(
			config, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance))
			continue;
		return &config;
	}
	return nullptr;
}

static const Bunny::TextureOverrideConfig *FindDescriptorTextureOverrideLocked(
	ID3D12GraphicsCommandList *commandList,
	const Bunny::CommandListTarget &target,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex)
{
	if (!commandList)
		return nullptr;

	std::vector<DX12CurrentShaderResourceBinding> bindings;
	const bool compute = TargetStageIsCompute(target.stage);
	if (!DX12BindingGetCurrentShaderResourceBindings(
		    commandList, compute, DescriptorRangeTypeForTarget(target.kind), &bindings))
		return nullptr;

	for (const DX12CurrentShaderResourceBinding &binding : bindings) {
		if (!DescriptorBindingMatchesTarget(binding, target))
			continue;
		const uint32_t hash = HashDescriptorBinding(binding);
		const Bunny::TextureOverrideConfig *config =
			FindTextureOverrideByHashLocked(
				hash, vertexCount, indexCount, instanceCount,
				firstVertex, firstIndex, 0);
		if (config)
			return config;
	}
	return nullptr;
}

static ID3D12Resource *SelectIaResourceForViewLocked(
	ID3D12GraphicsCommandList *commandList, DX12LoadedResource *resource)
{
	if (!resource)
		return nullptr;
	if (resource->uavResource && resource->uavValid) {
		if (resource->uavBarrierPending) {
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			barrier.UAV.pResource = resource->uavResource;
			commandList->ResourceBarrier(1, &barrier);
			resource->uavBarrierPending = false;
		}
		if (resource->uavState != D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) {
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource->uavResource;
			barrier.Transition.StateBefore = resource->uavState;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commandList->ResourceBarrier(1, &barrier);
			resource->uavState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		}
		return resource->uavResource;
	}
	if (resource->srvResource && resource->srvByteWidth > resource->byteWidth)
		return resource->srvResource;
	return resource->resource;
}

static UINT IaViewByteWidthForResource(const DX12LoadedResource &resource, ID3D12Resource *iaResource)
{
	UINT64 byteWidth = resource.byteWidth;
	if (resource.uavResource && iaResource == resource.uavResource && resource.uavByteWidth)
		byteWidth = resource.uavByteWidth;
	else if (resource.srvResource && iaResource == resource.srvResource && resource.srvByteWidth)
		byteWidth = resource.srvByteWidth;
	return static_cast<UINT>((std::min)(byteWidth, static_cast<UINT64>(UINT_MAX)));
}
static void LogIaResourceBindLimited(
	const char *target, uint32_t slot, const DX12LoadedResource &resource,
	ID3D12Resource *iaResource)
{
#if defined(_DEBUG)
	const bool usingUav = resource.uavResource && iaResource == resource.uavResource;
	const bool usingSrv = resource.srvResource && iaResource == resource.srvResource;
	DX12LogDebugJsonFunc("DX12IaResourceBind",
		"\"target\":\"%s\",\"slot\":%u,\"resource\":\"%S\",\"source\":\"%s\",\"bytes\":%llu,\"viewBytes\":%u,\"uavBytes\":%llu,\"stride\":%u,\"gpuVa\":\"0x%llx\"",
		target ? target : "", slot, resource.name.c_str(),
		usingUav ? "uav" : (usingSrv ? "srv" : "upload"),
		static_cast<unsigned long long>(resource.byteWidth),
		IaViewByteWidthForResource(resource, iaResource),
		static_cast<unsigned long long>(resource.uavByteWidth),
		resource.stride,
		iaResource ? static_cast<unsigned long long>(iaResource->GetGPUVirtualAddress()) : 0ull);
#else
	(void)target;
	(void)slot;
	(void)resource;
	(void)iaResource;
#endif
}

static bool AppendResourceViewsLocked(
	ID3D12Device *device, ID3D12GraphicsCommandList *commandList, const DX12IaHashState &iaState,
	const std::wstring &indexResource,
	const std::vector<DX12CompiledVertexResourceBinding> &vertexResources,
	DX12ModIaReplacement *replacement)
{
	if (!device || !commandList || !replacement)
		return false;

	bool changed = false;
	if (!indexResource.empty()) {
		DX12LoadedResource *resource = EnsureLoadedResourceLocked(device, indexResource);
		ID3D12Resource *iaResource = SelectIaResourceForViewLocked(commandList, resource);
		if (resource && iaResource) {
			replacement->indexBuffer.BufferLocation = iaResource->GetGPUVirtualAddress();
			replacement->indexBuffer.SizeInBytes =
				IaViewByteWidthForResource(*resource, iaResource);
			replacement->indexBuffer.Format =
				resource->format == DXGI_FORMAT_R16_UINT ||
				resource->format == DXGI_FORMAT_R32_UINT ?
				resource->format :
				(iaState.hasIndexBuffer && iaState.indexView.Format != DXGI_FORMAT_UNKNOWN ?
					iaState.indexView.Format : DXGI_FORMAT_R32_UINT);
			replacement->RetainResource(iaResource);
			replacement->hasIndexBuffer = true;
			LogIaResourceBindLimited("ib", 0, *resource, iaResource);
			changed = true;
		}
	}

	if (!vertexResources.empty()) {
		uint32_t minSlot = vertexResources.front().slot;
		uint32_t maxSlot = vertexResources.back().slot;
		if (maxSlot >= minSlot && maxSlot - minSlot < 32) {
			if (replacement->vertexBuffers.empty() ||
			    minSlot < replacement->vertexBufferStartSlot ||
			    maxSlot >= replacement->vertexBufferStartSlot + replacement->vertexBuffers.size()) {
				uint32_t oldStart = replacement->vertexBuffers.empty() ?
					minSlot : replacement->vertexBufferStartSlot;
				uint32_t oldEnd = replacement->vertexBuffers.empty() ?
					maxSlot : replacement->vertexBufferStartSlot +
						static_cast<uint32_t>(replacement->vertexBuffers.size()) - 1;
				uint32_t newStart = (std::min)(oldStart, minSlot);
				uint32_t newEnd = (std::max)(oldEnd, maxSlot);
				if (newEnd - newStart >= 32)
					return changed;

				std::vector<D3D12_VERTEX_BUFFER_VIEW> resized(newEnd - newStart + 1);
				for (size_t i = 0; i < replacement->vertexBuffers.size(); ++i) {
					uint32_t slot = replacement->vertexBufferStartSlot + static_cast<uint32_t>(i);
					resized[slot - newStart] = replacement->vertexBuffers[i];
				}
				replacement->vertexBufferStartSlot = newStart;
				replacement->vertexBuffers.swap(resized);
			}

			for (const auto &item : vertexResources) {
				DX12LoadedResource *resource = EnsureLoadedResourceLocked(device, item.resource);
				ID3D12Resource *iaResource = SelectIaResourceForViewLocked(commandList, resource);
				if (!resource || !iaResource)
					continue;
				D3D12_VERTEX_BUFFER_VIEW &view =
					replacement->vertexBuffers[item.slot - replacement->vertexBufferStartSlot];
				view.BufferLocation = iaResource->GetGPUVirtualAddress();
				view.SizeInBytes = IaViewByteWidthForResource(*resource, iaResource);
				const DX12IaBufferHash *sourceSlot = FindIaVertexSlot(iaState, item.slot);
				view.StrideInBytes = resource->stride ? resource->stride :
					(sourceSlot ? sourceSlot->vertexView.StrideInBytes : 0);
				replacement->RetainResource(iaResource);
				LogIaResourceBindLimited("vb", item.slot, *resource, iaResource);
				changed = true;
			}
		}
	}
	return changed;
}

static const Bunny::TextureOverrideConfig *FindTargetTextureOverrideLocked(
	ID3D12GraphicsCommandList *commandList,
	const DX12IaHashState &iaState, const Bunny::CommandListTarget &target,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex)
{
	if (target.kind == Bunny::CommandListTargetKind::IndexBuffer) {
		if (!iaState.hasIndexBuffer || !iaState.indexHash)
			return nullptr;
		return FindTextureOverrideLocked(
			iaState.indexHash, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, 0, false, true, 0);
	}

	if (target.kind == Bunny::CommandListTargetKind::VertexBuffer) {
		const DX12IaBufferHash *slot = FindIaVertexSlot(iaState, target.slot);
		if (!slot || !slot->hash)
			return nullptr;
		return FindTextureOverrideLocked(
			slot->hash, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, 0, false, false, target.slot);
	}

	if (target.kind == Bunny::CommandListTargetKind::ConstantBuffer ||
	    target.kind == Bunny::CommandListTargetKind::ShaderResource ||
	    target.kind == Bunny::CommandListTargetKind::UnorderedAccessView) {
		return FindDescriptorTextureOverrideLocked(
			commandList, target, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex);
	}

	return nullptr;
}
