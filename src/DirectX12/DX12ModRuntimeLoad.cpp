struct DX12StoredPso
{
	DX12PsoKind kind = DX12PsoKind::Graphics;
	ID3D12Device *device = nullptr;
	ID3D12RootSignature *graphicsRootSignature = nullptr;
	ID3D12RootSignature *computeRootSignature = nullptr;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc = {};
	D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc = {};
	std::vector<unsigned char> vsBytecode;
	std::vector<unsigned char> psBytecode;
	std::vector<unsigned char> dsBytecode;
	std::vector<unsigned char> hsBytecode;
	std::vector<unsigned char> gsBytecode;
	std::vector<unsigned char> csBytecode;
	ID3D12PipelineState *replacement = nullptr;
	UINT64 replacementGeneration = 0;
	bool skipDraw = false;
	bool skipDispatch = false;
	UINT64 skipGeneration = 0;
};

static std::unordered_map<ID3D12PipelineState*, DX12StoredPso> gPsoRecords;

static void ReleaseLoadedResourcesLocked();
static bool ResourceConfigLooksLikeBuffer(const Bunny::ResourceConfig &config);

static std::vector<DX12VertexLimitRaiseConfig> CollectVertexLimitRaiseConfigs(
	const Bunny::TextureOverrideMap &textureOverrides)
{
	std::vector<DX12VertexLimitRaiseConfig> configs;
	for (const auto &bucket : textureOverrides) {
		for (const Bunny::TextureOverrideConfig &config : bucket.second) {
			if (!config.hasVertexLimitRaise || !config.overrideByteWidth)
				continue;
			DX12VertexLimitRaiseConfig out;
			out.section = config.originalSection.empty() ? config.section : config.originalSection;
			out.hasMatchCs = config.hasMatchCs;
			out.matchCs = config.matchCs;
			out.overrideByteStride = config.overrideByteStride;
			out.overrideVertexCount = config.overrideVertexCount;
			out.overrideByteWidth = config.overrideByteWidth;
			out.uavByteStride = config.uavByteStride;
			out.overrideNumElements = config.overrideNumElements;
			configs.push_back(out);
		}
	}
	return configs;
}

static bool IsComputePreSkinTextureOverride(const Bunny::TextureOverrideConfig &config)
{
	return config.hasMatchCs && !config.preSkinMatchCsSrvHashes.empty() &&
		!config.preSkinCsSrvResources.empty();
}

static void BuildDx12PreSkinTextureOverrideIndex(
	const Bunny::TextureOverrideMap &textureOverrides,
	std::vector<DX12PreSkinTextureOverrideCandidate> *preSkinTextureOverrides,
	std::unordered_map<uint32_t, std::vector<size_t>> *uavHashTextureOverrideIndex,
	std::unordered_map<uint64_t, std::vector<size_t>> *matchCsTextureOverrideIndex)
{
	if (!preSkinTextureOverrides || !uavHashTextureOverrideIndex ||
	    !matchCsTextureOverrideIndex)
		return;

	preSkinTextureOverrides->clear();
	uavHashTextureOverrideIndex->clear();
	matchCsTextureOverrideIndex->clear();
	UINT64 order = 0;
	for (const auto &bucket : textureOverrides) {
		for (const Bunny::TextureOverrideConfig &config : bucket.second) {
			if (!IsComputePreSkinTextureOverride(config))
				continue;

			DX12PreSkinTextureOverrideCandidate candidate;
			candidate.config = config;
			candidate.hash = bucket.first;
			candidate.order = order++;
			const size_t index = preSkinTextureOverrides->size();
			preSkinTextureOverrides->push_back(std::move(candidate));
			(*matchCsTextureOverrideIndex)[config.matchCs].push_back(index);
		}
	}
}

static void BuildDx12ExplicitMatchCsResourceIndex(
	const Bunny::TextureOverrideMap &textureOverrides,
	std::unordered_map<std::wstring, std::vector<std::wstring>> *resourceSections)
{
	if (!resourceSections)
		return;

	resourceSections->clear();
	for (const auto &bucket : textureOverrides) {
		for (const Bunny::TextureOverrideConfig &config : bucket.second) {
			if (!config.hasMatchCs)
				continue;
			for (const auto &item : config.preSkinCsSrvResources) {
				if (item.second.empty())
					continue;
				(*resourceSections)[Bunny::ToLower(item.second)].push_back(config.section);
			}
		}
	}
}

static bool HasPreSkinTextureOverrideCandidates(
	const Bunny::TextureOverrideMap &textureOverrides,
	std::unordered_set<uint64_t> *matchCsHashes,
	bool *hasVlrWithoutMatchCs,
	bool *hasExplicitMatchCsUavBytes)
{
	if (matchCsHashes)
		matchCsHashes->clear();
	if (hasVlrWithoutMatchCs)
		*hasVlrWithoutMatchCs = false;
	if (hasExplicitMatchCsUavBytes)
		*hasExplicitMatchCsUavBytes = false;

	bool found = false;
	for (const auto &bucket : textureOverrides) {
		for (const Bunny::TextureOverrideConfig &config : bucket.second) {
			if (IsComputePreSkinTextureOverride(config)) {
				found = true;
				if (matchCsHashes)
					matchCsHashes->insert(config.matchCs);
				continue;
			}
			if (config.hasVertexLimitRaise && config.overrideByteWidth) {
				found = true;
				if (hasVlrWithoutMatchCs)
					*hasVlrWithoutMatchCs = true;
			}
		}
	}
	return found;
}

static bool ResourceUsedByExplicitMatchCsLocked(const std::wstring &resourceName)
{
	if (resourceName.empty())
		return false;
	return gExplicitMatchCsResourceSections.find(Bunny::ToLower(resourceName)) !=
		gExplicitMatchCsResourceSections.end();
}

static bool ResourceBlockedByInactiveExplicitMatchCsLocked(const std::wstring &resourceName)
{
	if (resourceName.empty())
		return false;

	auto indexed = gExplicitMatchCsResourceSections.find(Bunny::ToLower(resourceName));
	if (indexed == gExplicitMatchCsResourceSections.end())
		return false;

	for (const std::wstring &section : indexed->second) {
		AcquireSRWLockShared(&gPreSkinLock);
		const bool activeExplicitMatchCs =
			gActivePreSkinTextureOverrides.find(section) !=
			gActivePreSkinTextureOverrides.end();
		ReleaseSRWLockShared(&gPreSkinLock);
		if (activeExplicitMatchCs)
			return false;
	}
	return true;
}

static void BuildDx12VlrResourceCandidates(
	const Bunny::ResourceMap &resources,
	const std::unordered_map<std::wstring, std::vector<std::wstring>> &explicitMatchCsResourceSections,
	std::vector<std::wstring> *resourceCandidates)
{
	if (!resourceCandidates)
		return;

	resourceCandidates->clear();
	std::unordered_set<std::wstring> seen;
	for (const auto &item : resources) {
		if (!ResourceConfigLooksLikeBuffer(item.second))
			continue;
		if (explicitMatchCsResourceSections.find(Bunny::ToLower(item.first)) !=
		    explicitMatchCsResourceSections.end())
			continue;
		if (!seen.insert(item.first).second)
			continue;
		resourceCandidates->push_back(item.first);
	}
}

static void ClearPreSkinRuntimeState()
{
	AcquireSRWLockExclusive(&gPreSkinLock);
	gRecentComputeUavs.clear();
	gComputeUavSerial = 0;
	gPatchedPreSkinDescriptors.clear();
	gKnownPreSkinDescriptorPatches.clear();
	ClearActivePreSkinTextureOverridesLocked();
	gPreSkinCbvReadCache.clear();
	gPreSkinSrvNegativeCache.clear();
	gPreSkinSrvStableNegativeCache.clear();
	gPreSkinDescriptorPatchAbortCache.clear();
	gPreSkinSrvPositiveCache.clear();
	gPreSkinSrvStablePositiveCache.clear();
	InterlockedExchange(&gPreSkinDescriptorAbortCacheHitLogCount, 0);
	++gPreSkinSrvCacheGeneration;
	ReleaseSRWLockExclusive(&gPreSkinLock);

	DX12PreSkinReleaseBatch releaseBatch;
	AcquireSRWLockExclusive(&gPreSkinRetireLock);
	for (auto &pending : gPendingPreSkinSubmissions)
		MovePendingPreSkinSubmissionToReleaseBatch(&pending.second, &releaseBatch);
	gPendingPreSkinSubmissions.clear();
	for (auto &retired : gRetiredPreSkinResources) {
		if (retired.resource)
			releaseBatch.resources.push_back(retired.resource);
	}
	gRetiredPreSkinResources.clear();
	for (auto &retired : gRetiredPreSkinDescriptorHeaps) {
		if (retired.heap)
			releaseBatch.descriptorHeaps.push_back(retired.heap);
	}
	gRetiredPreSkinDescriptorHeaps.clear();
	ReleaseSRWLockExclusive(&gPreSkinRetireLock);

	AcquireSRWLockExclusive(&gPreSkinDescriptorRingLock);
	ReleasePreSkinDescriptorRingLocked(&releaseBatch);
	ReleaseSRWLockExclusive(&gPreSkinDescriptorRingLock);
	ReleasePreSkinReleaseBatch(&releaseBatch);
}

static bool DescriptorOverlapsView(
	const DX12DescriptorSummary &descriptor, UINT64 gpuVa, UINT64 size)
{
	if (!descriptor.resource || gpuVa == 0 || size == 0)
		return false;

	UINT64 descriptorBegin = descriptor.gpuVirtualAddress + descriptor.resourceOffset;
	UINT64 descriptorSize = descriptor.viewSize;
	if (!descriptorSize && descriptor.hasResourceDesc &&
		descriptor.resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		descriptorSize = descriptor.resourceDesc.Width;
	if (!descriptorSize)
		return false;

	const UINT64 descriptorEnd = descriptorBegin + descriptorSize;
	const UINT64 viewEnd = gpuVa + size;
	return gpuVa < descriptorEnd && descriptorBegin < viewEnd;
}

static bool ProducerMatchesIaBuffer(
	const DX12ComputeUavProducer &producer, const DX12IaBufferHash &buffer)
{
	if (!producer.hasDescriptor)
		return false;
	const D3D12_VERTEX_BUFFER_VIEW &view = buffer.vertexView;
	return DescriptorOverlapsView(producer.descriptor, view.BufferLocation, view.SizeInBytes);
}

static bool ProducerMatchesReplacementTarget(
	const DX12ComputeUavProducer &producer, const DX12VertexLimitRaiseConfig &vlr)
{
	if (!producer.hasDescriptor)
		return false;
	const DX12DescriptorSummary &descriptor = producer.descriptor;
	if (descriptor.kind != "UAV" || descriptor.viewDimension != D3D12_UAV_DIMENSION_BUFFER)
		return false;
	if (vlr.uavByteStride && descriptor.structureByteStride &&
		descriptor.structureByteStride != vlr.uavByteStride)
		return false;
	if (vlr.overrideByteWidth && descriptor.viewSize &&
		descriptor.viewSize + static_cast<UINT64>(vlr.overrideByteStride) * 4096 < vlr.overrideByteWidth)
		return false;
	return true;
}

static void SetBasePaths(const wchar_t *configPath)
{
	gConfigPath = configPath ? configPath : L"";
	gBaseDir = gConfigPath;
	if (!gBaseDir.empty()) {
		wchar_t path[MAX_PATH];
		wcsncpy_s(path, gBaseDir.c_str(), _TRUNCATE);
		PathRemoveFileSpecW(path);
		gBaseDir = path;
	}
	if (gBaseDir.empty())
		gBaseDir = L".";

	wchar_t shaderFixes[MAX_PATH];
	wcsncpy_s(shaderFixes, gBaseDir.c_str(), _TRUNCATE);
	PathAppendW(shaderFixes, L"ShaderFixes");
	gShaderFixesDir = shaderFixes;
}

static bool ReadFileBytes(const wchar_t *path, std::vector<unsigned char> *data)
{
	if (!path || !data)
		return false;

	FILE *file = _wfsopen(path, L"rb", _SH_DENYNO);
	if (!file)
		return false;

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return false;
	}
	long size = ftell(file);
	if (size <= 0) {
		fclose(file);
		return false;
	}
	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return false;
	}

	data->resize(static_cast<size_t>(size));
	size_t read = fread(data->data(), 1, data->size(), file);
	fclose(file);
	if (read != data->size()) {
		data->clear();
		return false;
	}
	return true;
}

static void ReleaseStoredPso(DX12StoredPso *record)
{
	if (!record)
		return;
	if (record->replacement) {
		record->replacement->Release();
		record->replacement = nullptr;
	}
	if (record->device) {
		record->device->Release();
		record->device = nullptr;
	}
	if (record->graphicsRootSignature) {
		record->graphicsRootSignature->Release();
		record->graphicsRootSignature = nullptr;
	}
	if (record->computeRootSignature) {
		record->computeRootSignature->Release();
		record->computeRootSignature = nullptr;
	}
}

static void StoreShaderBytecode(
	const D3D12_SHADER_BYTECODE &source, std::vector<unsigned char> *storage,
	D3D12_SHADER_BYTECODE *target)
{
	if (!storage || !target)
		return;

	*target = {};
	storage->clear();
	if (!source.pShaderBytecode || source.BytecodeLength == 0)
		return;

	storage->resize(source.BytecodeLength);
	memcpy(storage->data(), source.pShaderBytecode, source.BytecodeLength);
	target->pShaderBytecode = storage->data();
	target->BytecodeLength = storage->size();
}

static void DeepCopyGraphicsDesc(
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC *source, DX12StoredPso *record)
{
	record->graphicsDesc = *source;
	record->graphicsRootSignature = source->pRootSignature;
	if (record->graphicsRootSignature)
		record->graphicsRootSignature->AddRef();
	StoreShaderBytecode(source->VS, &record->vsBytecode, &record->graphicsDesc.VS);
	StoreShaderBytecode(source->PS, &record->psBytecode, &record->graphicsDesc.PS);
	StoreShaderBytecode(source->DS, &record->dsBytecode, &record->graphicsDesc.DS);
	StoreShaderBytecode(source->HS, &record->hsBytecode, &record->graphicsDesc.HS);
	StoreShaderBytecode(source->GS, &record->gsBytecode, &record->graphicsDesc.GS);
}

static void DeepCopyComputeDesc(
	const D3D12_COMPUTE_PIPELINE_STATE_DESC *source, DX12StoredPso *record)
{
	record->computeDesc = *source;
	record->computeRootSignature = source->pRootSignature;
	if (record->computeRootSignature)
		record->computeRootSignature->AddRef();
	StoreShaderBytecode(source->CS, &record->csBytecode, &record->computeDesc.CS);
}

uint64_t DX12ModHashShaderBytecode(const void *data, size_t size)
{
	const unsigned char *bytes = static_cast<const unsigned char*>(data);
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

void DX12ModSetSafeMode(bool safeMode)
{
	InterlockedExchange(&gDx12SafeMode, safeMode ? 1 : 0);
}

static bool IsVlrBufferResizeCandidate(
	const D3D12_RESOURCE_DESC &desc, const DX12VertexLimitRaiseConfig &vlr)
{
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width == 0)
		return false;
	if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
		return false;
	if (desc.Width >= vlr.overrideByteWidth)
		return false;

	const UINT64 tolerance = std::max<UINT64>(
		static_cast<UINT64>(vlr.overrideByteStride) * 4096,
		vlr.overrideByteWidth / 4);
	if (desc.Width + tolerance < vlr.overrideByteWidth)
		return false;

	return true;
}

static bool AdjustBufferResourceDescCommon(D3D12_RESOURCE_DESC *desc, const char *source)
{
	if (!desc)
		return false;

	DX12VertexLimitRaiseConfig match;
	bool found = false;
	if (!TryAcquireSRWLockShared(&gModLock))
		return false;
	for (const DX12VertexLimitRaiseConfig &vlr : gVertexLimitRaiseConfigs) {
		if (!IsVlrBufferResizeCandidate(*desc, vlr))
			continue;
		if (!found || vlr.overrideByteWidth > match.overrideByteWidth) {
			match = vlr;
			found = true;
		}
	}
	ReleaseSRWLockShared(&gModLock);

	if (!found)
		return false;

#if defined(_DEBUG)
	const UINT64 oldWidth = desc->Width;
#endif
	desc->Width = match.overrideByteWidth;
#if defined(_DEBUG)
	DX12LogDebugJsonFunc("DX12VertexLimitRaiseResize",
		"\"source\":\"%s\",\"section\":\"%S\",\"oldWidth\":%llu,\"newWidth\":%llu,\"overrideByteStride\":%u,\"overrideVertexCount\":%u,\"flags\":%u",
		source ? source : "", match.section.c_str(),
		static_cast<unsigned long long>(oldWidth),
		static_cast<unsigned long long>(desc->Width),
		match.overrideByteStride, match.overrideVertexCount,
		static_cast<unsigned>(desc->Flags));
#endif
	return true;
}

bool DX12ModAdjustBufferResourceDesc(D3D12_RESOURCE_DESC *desc, const char *source)
{
	return AdjustBufferResourceDescCommon(desc, source);
}

bool DX12ModAdjustBufferResourceDesc1(D3D12_RESOURCE_DESC1 *desc, const char *source)
{
	if (!desc)
		return false;
	D3D12_RESOURCE_DESC desc0 = {};
	desc0.Dimension = desc->Dimension;
	desc0.Alignment = desc->Alignment;
	desc0.Width = desc->Width;
	desc0.Height = desc->Height;
	desc0.DepthOrArraySize = desc->DepthOrArraySize;
	desc0.MipLevels = desc->MipLevels;
	desc0.Format = desc->Format;
	desc0.SampleDesc = desc->SampleDesc;
	desc0.Layout = desc->Layout;
	desc0.Flags = desc->Flags;
	if (!AdjustBufferResourceDescCommon(&desc0, source))
		return false;
	desc->Width = desc0.Width;
	return true;
}

bool DX12ModAdjustUavDesc(
	ID3D12Resource *resource, D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, const char *source)
{
	if (!resource || !desc || desc->ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
		return false;

	UINT bytesPerElement = desc->Buffer.StructureByteStride;
	if (!bytesPerElement && (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW))
		bytesPerElement = 4;
	if (!bytesPerElement)
		return false;

	D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
	if (resourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
		return false;

	DX12VertexLimitRaiseConfig match;
	bool found = false;
	if (!TryAcquireSRWLockShared(&gModLock))
		return false;
	for (const DX12VertexLimitRaiseConfig &vlr : gVertexLimitRaiseConfigs) {
		if (!vlr.overrideNumElements || !vlr.uavByteStride)
			continue;
		if (bytesPerElement != vlr.uavByteStride)
			continue;
		if (desc->Buffer.NumElements >= vlr.overrideNumElements)
			continue;
		const UINT64 requiredBytes =
			(desc->Buffer.FirstElement + vlr.overrideNumElements) * bytesPerElement;
		if (resourceDesc.Width < requiredBytes)
			continue;
		if (!found || vlr.overrideNumElements > match.overrideNumElements) {
			match = vlr;
			found = true;
		}
	}
	ReleaseSRWLockShared(&gModLock);

	if (!found)
		return false;

#if defined(_DEBUG)
	const UINT oldNumElements = desc->Buffer.NumElements;
#endif
	desc->Buffer.NumElements = static_cast<UINT>(match.overrideNumElements);
#if defined(_DEBUG)
	DX12LogDebugJsonFunc("DX12VertexLimitRaiseUav",
		"\"source\":\"%s\",\"section\":\"%S\",\"oldNumElements\":%u,\"newNumElements\":%u,\"bytesPerElement\":%u,\"resourceWidth\":%llu",
		source ? source : "", match.section.c_str(), oldNumElements,
		desc->Buffer.NumElements, bytesPerElement,
		static_cast<unsigned long long>(resourceDesc.Width));
#endif
	return true;
}

static bool CommandListActionHasRuntimeEffect(
	const Bunny::CommandListAction &action,
	const Bunny::CommandListMap &commandLists,
	std::unordered_map<std::wstring, bool> *effectCache,
	std::set<std::wstring> *visiting);
static bool TextureOverrideMatchesIaBinding(
	const Bunny::TextureOverrideConfig &config, bool indexBuffer, uint32_t vertexSlot);
static void BuildDx12RelatedTextureOverrideIndex(
	const Bunny::TextureOverrideMap &textureOverrides,
	const std::unordered_map<std::wstring, bool> &textureOverridePreEffect,
	const std::unordered_map<std::wstring, uint32_t> &textureOverrideSectionIds,
	std::vector<DX12RelatedTextureOverrideCandidate> *relatedTextureOverrides,
	std::unordered_map<std::wstring, std::vector<size_t>> *relatedTokenIndex);
static void BuildDx12IaTextureOverrideIndex(
	const Bunny::TextureOverrideMap &textureOverrides,
	const std::unordered_map<std::wstring, uint32_t> &textureOverrideSectionIds,
	const std::unordered_map<std::wstring, bool> &textureOverridePreEffect,
	const std::unordered_map<std::wstring, bool> &textureOverridePostEffect,
	std::vector<DX12IaTextureOverrideCandidate> *iaTextureOverrides,
	std::unordered_map<uint32_t, std::vector<size_t>> *indexTextureOverrideIndex,
	std::unordered_map<UINT64, std::vector<size_t>> *vertexTextureOverrideIndex,
	std::unordered_map<uint32_t, std::vector<size_t>> *anyVertexTextureOverrideIndex,
	std::unordered_map<UINT64, std::vector<size_t>> *mergedVertexTextureOverrideIndex);
static void BuildDx12CompiledTextureOverridePlans(
	const Bunny::TextureOverrideMap &textureOverrides,
	const std::unordered_map<std::wstring, uint32_t> &textureOverrideSectionIds,
	const std::unordered_map<std::wstring, DX12CompiledCommandListPlan> &compiledCommandListPlans,
	std::unordered_map<uint32_t, DX12CompiledTextureOverridePlan> *compiledPlans);
static void BuildDx12CompiledCommandListPlans(
	const Bunny::CommandListMap &commandLists,
	std::unordered_map<std::wstring, DX12CompiledCommandListPlan> *compiledPlans);
static void BuildDx12PreSkinTextureOverrideIndex(
	const Bunny::TextureOverrideMap &textureOverrides,
	std::vector<DX12PreSkinTextureOverrideCandidate> *preSkinTextureOverrides,
	std::unordered_map<uint32_t, std::vector<size_t>> *uavHashTextureOverrideIndex,
	std::unordered_map<uint64_t, std::vector<size_t>> *matchCsTextureOverrideIndex);
static void BuildDx12ExplicitMatchCsResourceIndex(
	const Bunny::TextureOverrideMap &textureOverrides,
	std::unordered_map<std::wstring, std::vector<std::wstring>> *resourceSections);
static void BuildDx12VlrResourceCandidates(
	const Bunny::ResourceMap &resources,
	const std::unordered_map<std::wstring, std::vector<std::wstring>> &explicitMatchCsResourceSections,
	std::vector<std::wstring> *resourceCandidates);

static bool CommandListHasRuntimeEffect(
	const std::wstring &name,
	const Bunny::CommandListMap &commandLists,
	std::unordered_map<std::wstring, bool> *effectCache,
	std::set<std::wstring> *visiting)
{
	if (!effectCache || !visiting)
		return true;

	auto cached = effectCache->find(name);
	if (cached != effectCache->end())
		return cached->second;
	if (!visiting->insert(name).second)
		return true;

	bool effect = false;
	auto it = commandLists.find(name);
	if (it != commandLists.end()) {
		for (const Bunny::CommandListAction &action : it->second.actions) {
			if (CommandListActionHasRuntimeEffect(
				    action, commandLists, effectCache, visiting)) {
				effect = true;
				break;
			}
		}
	}
	visiting->erase(name);
	(*effectCache)[name] = effect;
	return effect;
}

static bool CommandListActionHasRuntimeEffect(
	const Bunny::CommandListAction &action,
	const Bunny::CommandListMap &commandLists,
	std::unordered_map<std::wstring, bool> *effectCache,
	std::set<std::wstring> *visiting)
{
	switch (action.kind) {
	case Bunny::CommandListActionKind::Run:
		return CommandListHasRuntimeEffect(
			action.commandList, commandLists, effectCache, visiting);
	case Bunny::CommandListActionKind::CheckTextureOverride:
	case Bunny::CommandListActionKind::HandlingSkip:
	case Bunny::CommandListActionKind::SetIndexBuffer:
	case Bunny::CommandListActionKind::SetVertexBuffer:
	case Bunny::CommandListActionKind::Draw:
	case Bunny::CommandListActionKind::DrawIndexed:
	case Bunny::CommandListActionKind::DrawFromCaller:
	case Bunny::CommandListActionKind::DrawIndexedFromCaller:
	case Bunny::CommandListActionKind::Dispatch:
		return true;
	}
	return true;
}

static bool CommandListLinksHaveRuntimeEffect(
	const std::vector<std::wstring> &links,
	const Bunny::CommandListMap &commandLists,
	std::unordered_map<std::wstring, bool> *effectCache)
{
	std::set<std::wstring> visiting;
	for (const std::wstring &link : links) {
		if (CommandListHasRuntimeEffect(link, commandLists, effectCache, &visiting))
			return true;
	}
	return false;
}

static bool TextureOverrideActionsHaveRuntimeEffect(
	const Bunny::TextureOverrideConfig &config,
	bool executeDrawActions,
	const Bunny::CommandListMap &commandLists,
	std::unordered_map<std::wstring, bool> *effectCache)
{
	if (config.actions.empty())
		return !config.indexBufferResource.empty() ||
			!config.vertexBufferResources.empty() ||
			(executeDrawActions && config.handlingSkip);

	std::set<std::wstring> visiting;
	for (const Bunny::CommandListAction &action : config.actions) {
		switch (action.kind) {
		case Bunny::CommandListActionKind::Run:
			if (CommandListHasRuntimeEffect(
				    action.commandList, commandLists, effectCache, &visiting))
				return true;
			break;
		case Bunny::CommandListActionKind::CheckTextureOverride:
		case Bunny::CommandListActionKind::SetIndexBuffer:
		case Bunny::CommandListActionKind::SetVertexBuffer:
		case Bunny::CommandListActionKind::Dispatch:
			return true;
		case Bunny::CommandListActionKind::HandlingSkip:
		case Bunny::CommandListActionKind::Draw:
		case Bunny::CommandListActionKind::DrawIndexed:
		case Bunny::CommandListActionKind::DrawFromCaller:
		case Bunny::CommandListActionKind::DrawIndexedFromCaller:
			if (executeDrawActions)
				return true;
			break;
		}
	}
	return false;
}

static void BuildDx12RuntimeEffectIndexes(
	const Bunny::TextureOverrideMap &textureOverrides,
	const Bunny::CommandListMap &commandLists,
	std::unordered_map<std::wstring, bool> *commandListEffect,
	std::unordered_map<std::wstring, bool> *textureOverridePreEffect,
	std::unordered_map<std::wstring, bool> *textureOverridePostEffect)
{
	if (!commandListEffect || !textureOverridePreEffect || !textureOverridePostEffect)
		return;

	commandListEffect->clear();
	textureOverridePreEffect->clear();
	textureOverridePostEffect->clear();

	for (const auto &item : commandLists) {
		std::set<std::wstring> visiting;
		CommandListHasRuntimeEffect(item.first, commandLists, commandListEffect, &visiting);
	}

	for (const auto &bucket : textureOverrides) {
		for (const Bunny::TextureOverrideConfig &config : bucket.second) {
			const bool preEffect =
				CommandListLinksHaveRuntimeEffect(
					config.commandLists.pre, commandLists, commandListEffect) ||
				CommandListLinksHaveRuntimeEffect(
					config.commandLists.main, commandLists, commandListEffect) ||
				TextureOverrideActionsHaveRuntimeEffect(
					config, true, commandLists, commandListEffect);
			const bool postEffect =
				CommandListLinksHaveRuntimeEffect(
					config.commandLists.post, commandLists, commandListEffect);
			(*textureOverridePreEffect)[config.section] = preEffect;
			(*textureOverridePostEffect)[config.section] = postEffect;
		}
	}
}

static bool LookupRuntimeEffect(
	const std::unordered_map<std::wstring, bool> &effects,
	const std::wstring &section)
{
	auto effect = effects.find(section);
	return effect == effects.end() || effect->second;
}

static void BuildDx12TextureOverrideSectionIds(
	const Bunny::TextureOverrideMap &textureOverrides,
	std::unordered_map<std::wstring, uint32_t> *sectionIds)
{
	if (!sectionIds)
		return;

	sectionIds->clear();
	uint32_t nextId = 1;
	for (const auto &bucket : textureOverrides) {
		for (const Bunny::TextureOverrideConfig &config : bucket.second) {
			if (sectionIds->find(config.section) != sectionIds->end())
				continue;
			(*sectionIds)[config.section] = nextId++;
		}
	}
}

bool DX12ModRuntimeLoad(
	const wchar_t *configPath,
	std::wstring *statusMessage,
	bool *hasWarnings,
	bool *hasErrors)
{
	Bunny::MigotoIniLoadResult iniLoad;
	Bunny::ShaderOverrideMap shaderOverrides;
	Bunny::TextureOverrideMap textureOverrides;
	Bunny::ResourceMap resources;
	Bunny::CommandListMap commandLists;
	std::unordered_map<std::wstring, bool> commandListRuntimeEffect;
	std::unordered_map<std::wstring, bool> textureOverridePreRuntimeEffect;
	std::unordered_map<std::wstring, bool> textureOverridePostRuntimeEffect;
	std::unordered_map<std::wstring, uint32_t> textureOverrideSectionIds;
	std::unordered_map<uint32_t, DX12CompiledTextureOverridePlan> compiledTextureOverridePlans;
	std::unordered_map<std::wstring, DX12CompiledCommandListPlan> compiledCommandListPlans;
	bool presentRuntimeEffect = false;
	std::vector<DX12RelatedTextureOverrideCandidate> relatedTextureOverrides;
	std::unordered_map<std::wstring, std::vector<size_t>> relatedTextureOverrideTokenIndex;
	std::vector<DX12IaTextureOverrideCandidate> iaTextureOverrides;
	std::unordered_map<uint32_t, std::vector<size_t>> iaIndexTextureOverrideIndex;
	std::unordered_map<UINT64, std::vector<size_t>> iaVertexTextureOverrideIndex;
	std::unordered_map<uint32_t, std::vector<size_t>> iaAnyVertexTextureOverrideIndex;
	std::unordered_map<UINT64, std::vector<size_t>> iaMergedVertexTextureOverrideIndex;
	std::vector<DX12PreSkinTextureOverrideCandidate> preSkinTextureOverrides;
	std::unordered_map<uint32_t, std::vector<size_t>> preSkinUavHashTextureOverrideIndex;
	std::unordered_map<uint64_t, std::vector<size_t>> preSkinMatchCsTextureOverrideIndex;
	std::unordered_map<std::wstring, std::vector<std::wstring>> explicitMatchCsResourceSections;
	std::vector<std::wstring> vlrResourceCandidates;
	std::unordered_set<uint64_t> preSkinMatchCsHashes;
	bool hasPreSkinVlrWithoutMatchCs = false;
	size_t loadedShaderOverrideCount = 0;
	size_t loadedTextureOverrideCount = 0;
	size_t loadedResourceCount = 0;
	size_t loadedCommandListCount = 0;
	bool safeModeActive = false;
	bool activeShaderOverrides = false;
	bool activeTextureOverrides = false;
	bool preSkinProbeEnabled = false;
	bool loadedPreSkinTextureOverrideCandidates = false;
	bool loadedPresentRuntimeEffect = false;
	std::wstring loadedShaderFixesDir;
	std::vector<DX12VertexLimitRaiseConfig> loadedVertexLimitRaiseConfigs;
	UINT64 loadedGeneration = 0;
	
	SetBasePaths(configPath);
	if (!Bunny::LoadMigotoIniWithIncludes(configPath, &iniLoad)) {
		DX12LogJsonFunc("DX12ModRuntime",
			"\"status\":\"config_load_failed\",\"path\":\"%S\",\"error\":\"%S\"",
			configPath ? configPath : L"", iniLoad.document.Error().c_str());
		if (statusMessage)
			*statusMessage = L"DX12 mod config load failed: " + iniLoad.document.Error();
		if (hasWarnings)
			*hasWarnings = true;
		if (hasErrors)
			*hasErrors = true;
		return false;
	}

	const bool hasIniErrors = !iniLoad.errors.empty();
	Bunny::ParseShaderOverrideSections(iniLoad.document, &shaderOverrides);
	Bunny::ParseTextureOverrideSections(iniLoad.document, &textureOverrides);
	Bunny::ParseResourceSections(iniLoad.document, &resources);
	Bunny::ParseCommandListSections(iniLoad.document, &commandLists);
	std::vector<DX12VertexLimitRaiseConfig> vertexLimitRaiseConfigs =
		CollectVertexLimitRaiseConfigs(textureOverrides);
	bool hasPreSkinTextureOverrideCandidates =
		HasPreSkinTextureOverrideCandidates(
			textureOverrides, &preSkinMatchCsHashes,
			&hasPreSkinVlrWithoutMatchCs,
			nullptr);
	BuildDx12RuntimeEffectIndexes(
		textureOverrides, commandLists,
		&commandListRuntimeEffect,
		&textureOverridePreRuntimeEffect,
		&textureOverridePostRuntimeEffect);
	{
		std::set<std::wstring> visiting;
		presentRuntimeEffect =
			CommandListHasRuntimeEffect(
				L"Present", commandLists, &commandListRuntimeEffect, &visiting);
	}
	BuildDx12TextureOverrideSectionIds(
		textureOverrides, &textureOverrideSectionIds);
	BuildDx12RelatedTextureOverrideIndex(
		textureOverrides, textureOverridePreRuntimeEffect, textureOverrideSectionIds,
		&relatedTextureOverrides, &relatedTextureOverrideTokenIndex);
	BuildDx12IaTextureOverrideIndex(
		textureOverrides,
		textureOverrideSectionIds,
		textureOverridePreRuntimeEffect,
		textureOverridePostRuntimeEffect,
		&iaTextureOverrides,
		&iaIndexTextureOverrideIndex,
		&iaVertexTextureOverrideIndex,
		&iaAnyVertexTextureOverrideIndex,
		&iaMergedVertexTextureOverrideIndex);
	BuildDx12CompiledCommandListPlans(
		commandLists, &compiledCommandListPlans);
	BuildDx12CompiledTextureOverridePlans(
		textureOverrides, textureOverrideSectionIds, compiledCommandListPlans,
		&compiledTextureOverridePlans);
	BuildDx12PreSkinTextureOverrideIndex(
		textureOverrides,
		&preSkinTextureOverrides,
		&preSkinUavHashTextureOverrideIndex,
		&preSkinMatchCsTextureOverrideIndex);
	BuildDx12ExplicitMatchCsResourceIndex(
		textureOverrides, &explicitMatchCsResourceSections);
	BuildDx12VlrResourceCandidates(
		resources, explicitMatchCsResourceSections, &vlrResourceCandidates);

	AcquireSRWLockExclusive(&gModLock);
	gShaderOverrides.swap(shaderOverrides);
	gTextureOverrides.swap(textureOverrides);
	gResources.swap(resources);
	gCommandLists.swap(commandLists);
	gVlrResourceCandidates.swap(vlrResourceCandidates);
	gCommandListRuntimeEffect.swap(commandListRuntimeEffect);
	gTextureOverridePreRuntimeEffect.swap(textureOverridePreRuntimeEffect);
	gTextureOverridePostRuntimeEffect.swap(textureOverridePostRuntimeEffect);
	gTextureOverrideSectionIds.swap(textureOverrideSectionIds);
	gRelatedTextureOverrides.swap(relatedTextureOverrides);
	gRelatedTextureOverrideTokenIndex.swap(relatedTextureOverrideTokenIndex);
	gIaTextureOverrides.swap(iaTextureOverrides);
	gIaIndexTextureOverrideIndex.swap(iaIndexTextureOverrideIndex);
	gIaVertexTextureOverrideIndex.swap(iaVertexTextureOverrideIndex);
	gIaAnyVertexTextureOverrideIndex.swap(iaAnyVertexTextureOverrideIndex);
	gIaMergedVertexTextureOverrideIndex.swap(iaMergedVertexTextureOverrideIndex);
	gCompiledCommandListPlans.swap(compiledCommandListPlans);
	gCompiledTextureOverridePlans.swap(compiledTextureOverridePlans);
	gPreSkinTextureOverrides.swap(preSkinTextureOverrides);
	gPreSkinUavHashTextureOverrideIndex.swap(preSkinUavHashTextureOverrideIndex);
	gPreSkinMatchCsTextureOverrideIndex.swap(preSkinMatchCsTextureOverrideIndex);
	gExplicitMatchCsResourceSections.swap(explicitMatchCsResourceSections);
	gVertexLimitRaiseConfigs.swap(vertexLimitRaiseConfigs);
	gPreSkinMatchCsHashes.swap(preSkinMatchCsHashes);
	gHasPreSkinVlrWithoutMatchCs = hasPreSkinVlrWithoutMatchCs;
	ReleaseLoadedResourcesLocked();
	gIaSkipCache.clear();
	gIaTextureCandidateCache.clear();
	gIaReplacementMatchCache.clear();
	gIaReplacementPreparedFrameCache.clear();
	gShaderOverridePsoMatchCache.clear();
	gIaReplacementPrepareCachePresent = -1;
	gPresentCommandListExecutedPresent = -1;
	gPreSkinSectionAppliedPresent.clear();
	gPreSkinCbvReadCache.clear();
	gPreSkinSrvNegativeCache.clear();
	gPreSkinSrvStableNegativeCache.clear();
	gPreSkinDescriptorPatchAbortCache.clear();
	gPreSkinSrvPositiveCache.clear();
	gPreSkinSrvStablePositiveCache.clear();
	InterlockedExchange(&gPreSkinDescriptorAbortCacheHitLogCount, 0);
	gPreSkinUavMatchCache.clear();
	gPreSkinMatchCsInputCache.clear();
	gPreSkinMatchCsProbeKeys.clear();
	InterlockedExchange(&gPreSkinMatchCsProbeLogCount, 0);
	++gPreSkinActiveGeneration;
	++gPreSkinSrvCacheGeneration;
	InterlockedExchange(&gHasShaderOverrides, gShaderOverrides.empty() ? 0 : 1);
	InterlockedExchange(&gHasTextureOverrides, gTextureOverrides.empty() ? 0 : 1);
	InterlockedExchange(
		&gHasPreSkinTextureOverrideCandidates,
		hasPreSkinTextureOverrideCandidates ? 1 : 0);
	InterlockedExchange(&gHasPresentRuntimeEffect, presentRuntimeEffect ? 1 : 0);
	gLoaded = true;
	++gReloadGeneration;
	loadedShaderOverrideCount = gShaderOverrides.size();
	loadedTextureOverrideCount = gTextureOverrides.size();
	loadedResourceCount = gResources.size();
	loadedCommandListCount = gCommandLists.size();
	loadedPreSkinTextureOverrideCandidates = hasPreSkinTextureOverrideCandidates;
	loadedPresentRuntimeEffect = presentRuntimeEffect;
	safeModeActive = InterlockedCompareExchange(&gDx12SafeMode, 0, 0) != 0;
	activeShaderOverrides = !safeModeActive && !gShaderOverrides.empty();
	activeTextureOverrides = !safeModeActive && !gTextureOverrides.empty();
	preSkinProbeEnabled =
		!safeModeActive && !gTextureOverrides.empty() && hasPreSkinTextureOverrideCandidates;
	loadedShaderFixesDir = gShaderFixesDir;
	loadedVertexLimitRaiseConfigs = gVertexLimitRaiseConfigs;
	loadedGeneration = gReloadGeneration;
	for (auto &item : gPsoRecords) {
		if (item.second.replacement) {
			item.second.replacement->Release();
			item.second.replacement = nullptr;
		}
		item.second.replacementGeneration = 0;
		item.second.skipGeneration = 0;
	}
	ReleaseSRWLockExclusive(&gModLock);
	ClearPreSkinRuntimeState();

	DX12LogJsonFunc("DX12ModRuntime",
		"\"status\":\"loaded\",\"path\":\"%S\",\"iniFiles\":%zu,\"errors\":%zu,\"warnings\":%zu,\"shaderOverrides\":%zu,\"textureOverrides\":%zu,\"resources\":%zu,\"commandLists\":%zu,\"safeMode\":%s,\"activeShaderOverrides\":%s,\"activeTextureOverrides\":%s,\"preSkinCandidates\":%s,\"preSkinProbeEnabled\":%s,\"presentRuntimeEffect\":%s,\"shaderFixes\":\"%S\",\"generation\":%llu",
		configPath ? configPath : L"", iniLoad.loadedFiles.size(), iniLoad.errors.size(), iniLoad.warnings.size(),
		loadedShaderOverrideCount, loadedTextureOverrideCount, loadedResourceCount, loadedCommandListCount,
		safeModeActive ? "true" : "false",
		activeShaderOverrides ? "true" : "false",
		activeTextureOverrides ? "true" : "false",
		loadedPreSkinTextureOverrideCandidates ? "true" : "false",
		preSkinProbeEnabled ? "true" : "false",
		loadedPresentRuntimeEffect ? "true" : "false",
		loadedShaderFixesDir.c_str(),
		static_cast<unsigned long long>(loadedGeneration));
	if (safeModeActive &&
	    (loadedShaderOverrideCount || loadedTextureOverrideCount || loadedPresentRuntimeEffect)) {
		DX12LogJsonFunc("DX12ModRuntimeDisabled",
			"\"reason\":\"safe_mode\",\"shaderOverrides\":%zu,\"textureOverrides\":%zu,\"preSkinCandidates\":%s,\"presentRuntimeEffect\":%s",
			loadedShaderOverrideCount, loadedTextureOverrideCount,
			loadedPreSkinTextureOverrideCandidates ? "true" : "false",
			loadedPresentRuntimeEffect ? "true" : "false");
	}
	for (const std::wstring &loadedFile : iniLoad.loadedFiles) {
		DX12LogJsonFunc("DX12ModRuntimeIni",
			"\"status\":\"loaded\",\"path\":\"%S\"", loadedFile.c_str());
	}
	for (const std::wstring &error : iniLoad.errors) {
		DX12LogJsonFunc("DX12ModRuntimeIni",
			"\"status\":\"error\",\"message\":\"%S\"", error.c_str());
	}
	for (const std::wstring &warning : iniLoad.warnings) {
		DX12LogJsonFunc("DX12ModRuntimeIni",
			"\"status\":\"warning\",\"message\":\"%S\"", warning.c_str());
	}
	for (const DX12VertexLimitRaiseConfig &vlr : loadedVertexLimitRaiseConfigs) {
		DX12LogJsonFunc("DX12VertexLimitRaiseConfig",
			"\"section\":\"%S\",\"overrideByteStride\":%u,\"overrideVertexCount\":%u,\"overrideByteWidth\":%llu,\"uavByteStride\":%u,\"overrideNumElements\":%llu",
			vlr.section.c_str(), vlr.overrideByteStride, vlr.overrideVertexCount,
			static_cast<unsigned long long>(vlr.overrideByteWidth),
			vlr.uavByteStride, static_cast<unsigned long long>(vlr.overrideNumElements));
	}
	if (statusMessage) {
		wchar_t text[256];
		if (hasIniErrors) {
			swprintf_s(text, L"F10 DX12 mod config reloaded with %zu error(s)",
				iniLoad.errors.size());
		} else if (iniLoad.warnings.empty()) {
			swprintf_s(text, L"F10 DX12 mod config reloaded: %zu ini, %zu TextureOverride, %zu Resource",
				iniLoad.loadedFiles.size(), loadedTextureOverrideCount, loadedResourceCount);
		} else {
			swprintf_s(text, L"F10 DX12 mod config reloaded with %zu warning(s)",
				iniLoad.warnings.size());
		}
		*statusMessage = text;
	}
	if (hasWarnings)
		*hasWarnings = hasIniErrors || !iniLoad.warnings.empty();
	if (hasErrors)
		*hasErrors = hasIniErrors;
	return true;
}

bool DX12ModRuntimeReload()
{
	std::wstring configPath;
	AcquireSRWLockShared(&gModLock);
	configPath = gConfigPath;
	ReleaseSRWLockShared(&gModLock);
	if (configPath.empty()) {
		DX12LogJsonFunc("DX12ModRuntimeReload", "\"status\":\"skipped\",\"reason\":\"empty_config_path\"");
		return false;
	}
	DX12LogJsonFunc("DX12ModRuntimeReload", "\"status\":\"begin\",\"path\":\"%S\"", configPath.c_str());
	std::wstring status;
	bool hasWarnings = false;
	bool hasErrors = false;
	if (!DX12ModRuntimeLoad(configPath.c_str(), &status, &hasWarnings, &hasErrors)) {
		DX12SetOverlayError(status.c_str());
		return false;
	}
	if (hasErrors)
		DX12SetOverlayError(status.c_str());
	else if (hasWarnings)
		DX12SetOverlayWarning(status.c_str());
	else
		DX12SetOverlayStatus(status.c_str());
	return true;
}

static std::wstring ResolveResourcePath(
	const Bunny::ResourceConfig &config, const std::wstring &baseDir,
	bool rootFallback)
{
	if (config.filename.empty())
		return L"";

	wchar_t path[MAX_PATH];
	wcsncpy_s(path, config.filename.c_str(), _TRUNCATE);
	if (PathIsRelativeW(path)) {
		wchar_t combined[MAX_PATH];
		const std::wstring &base = rootFallback || config.sourceDir.empty() ? baseDir : config.sourceDir;
		wcsncpy_s(combined, base.c_str(), _TRUNCATE);
		PathAppendW(combined, config.filename.c_str());
		return combined;
	}
	return path;
}

static std::wstring ResolveResourcePathLocked(
	const Bunny::ResourceConfig &config, bool rootFallback)
{
	return ResolveResourcePath(config, gBaseDir, rootFallback);
}

static void LogIaFallbackPathLimited(const char *kind, const std::wstring &section, bool inserted)
{
	const LONG count = InterlockedIncrement(&gIaFallbackPathLogCount);
	if (count > kIaFallbackPathLogLimit)
		return;
	DX12LogDebugJsonFunc("DX12FallbackPath",
		"\"kind\":\"%s\",\"api\":\"DX12ModRuntime::IAOverride\",\"present\":%ld,"
		"\"section\":\"%S\",\"inserted\":%s,\"logIndex\":%ld",
		kind ? kind : "", DX12GetPresentCount(), section.c_str(),
		inserted ? "true" : "false", count);
}

static bool ResourceConfigLooksLikeBuffer(const Bunny::ResourceConfig &config)
{
	std::wstring type = Bunny::ToLower(Bunny::Trim(config.type));
	if (type.empty())
		return true;
	return type == L"buffer" ||
		type == L"structuredbuffer" ||
		type == L"appendstructuredbuffer" ||
		type == L"consumestructuredbuffer" ||
		type == L"byteaddressbuffer" ||
		type == L"rwbuffer" ||
		type == L"rwstructuredbuffer" ||
		type == L"rwbyteaddressbuffer";
}

static bool LoadResourceBytes(
	const Bunny::ResourceConfig &config, const std::wstring &baseDir,
	std::vector<unsigned char> *bytes, std::wstring *path)
{
	if (!bytes)
		return false;
	bytes->clear();

	std::wstring resolvedPath = ResolveResourcePath(config, baseDir, false);
	if (!resolvedPath.empty()) {
		if (path)
			*path = resolvedPath;
		if (ReadFileBytes(resolvedPath.c_str(), bytes))
			return true;

		if (!config.sourceDir.empty()) {
			std::wstring fallbackPath = ResolveResourcePath(config, baseDir, true);
			if (fallbackPath != resolvedPath) {
				if (path)
					*path = fallbackPath;
				const bool fallbackOk = ReadFileBytes(fallbackPath.c_str(), bytes);
				DX12LogJsonFunc("DX12FallbackPath",
					"\"kind\":\"resource_path_root\",\"api\":\"DX12ModRuntime::LoadResourceBytes\","
					"\"section\":\"%S\",\"primary\":\"%S\",\"fallback\":\"%S\",\"success\":%s",
					config.section.c_str(), resolvedPath.c_str(), fallbackPath.c_str(),
					fallbackOk ? "true" : "false");
				return fallbackOk;
			}
		}
		return false;
	}

	if (config.data.empty())
		return false;

	std::wstring data = Bunny::Trim(config.data);
	if (data.rfind(L"0x", 0) == 0 || data.rfind(L"0X", 0) == 0)
		data = data.substr(2);
	data.erase(std::remove_if(data.begin(), data.end(), iswspace), data.end());
	if (data.empty() || (data.size() % 2) != 0)
		return false;

	for (size_t i = 0; i < data.size(); i += 2) {
		wchar_t text[3] = {data[i], data[i + 1], 0};
		wchar_t *end = nullptr;
		unsigned long parsed = wcstoul(text, &end, 16);
		if (!end || *end)
			return false;
		bytes->push_back(static_cast<unsigned char>(parsed));
	}
	return !bytes->empty();
}

static bool LoadResourceBytesLocked(
	const Bunny::ResourceConfig &config, std::vector<unsigned char> *bytes, std::wstring *path)
{
	return LoadResourceBytes(config, gBaseDir, bytes, path);
}

static ID3D12Device *AcquireModDevice(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return nullptr;

	ID3D12Device *device = nullptr;
	if (SUCCEEDED(commandList->GetDevice(IID_PPV_ARGS(&device))) && device)
		return device;
	return nullptr;
}

static DX12LoadedResource *EnsureLoadedResourceLocked(
	ID3D12Device *device, const std::wstring &name)
{
	if (!device || name.empty())
		return nullptr;

	auto loaded = gLoadedResources.find(name);
	if (loaded != gLoadedResources.end())
		return loaded->second.resource && !loaded->second.failed ? &loaded->second : nullptr;

	auto configIt = gResources.find(name);
	if (configIt == gResources.end())
		return nullptr;

	const Bunny::ResourceConfig &config = configIt->second;
	if (!ResourceConfigLooksLikeBuffer(config))
		return nullptr;

	std::vector<unsigned char> bytes;
	std::wstring path;
	if (!LoadResourceBytesLocked(config, &bytes, &path)) {
		DX12LoadedResource failedResource;
		failedResource.failed = true;
		failedResource.name = name;
		failedResource.path = path;
		gLoadedResources[name] = failedResource;
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"read_failed\",\"path\":\"%S\"",
			config.section.c_str(), path.c_str());
		wchar_t status[512];
		swprintf_s(status, L"DX12 mod warning: failed to read resource\n%ls\n%ls",
			config.section.c_str(), path.c_str());
		DX12SetOverlayWarning(status);
		return nullptr;
	}

	UINT64 byteWidth = config.hasByteWidth ? config.byteWidth : bytes.size();
	if (byteWidth < bytes.size())
		byteWidth = bytes.size();
	if (byteWidth == 0)
		return nullptr;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = byteWidth;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ID3D12Resource *resource = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr, IID_PPV_ARGS(&resource));
	if (FAILED(hr) || !resource) {
		DX12LoadedResource failedResource;
		failedResource.failed = true;
		failedResource.name = name;
		failedResource.path = path;
		gLoadedResources[name] = failedResource;
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"create_failed\",\"hr\":\"0x%lx\",\"bytes\":%llu",
			config.section.c_str(), hr, static_cast<unsigned long long>(byteWidth));
		wchar_t status[512];
		swprintf_s(status, L"DX12 mod error: failed to create resource\n%ls hr=0x%lx",
			config.section.c_str(), hr);
		DX12SetOverlayError(status);
		return nullptr;
	}

	void *mapped = nullptr;
	D3D12_RANGE readRange = {};
	hr = resource->Map(0, &readRange, &mapped);
	if (SUCCEEDED(hr) && mapped) {
		memcpy(mapped, bytes.data(), bytes.size());
		if (byteWidth > bytes.size())
			memset(static_cast<unsigned char*>(mapped) + bytes.size(), 0, byteWidth - bytes.size());
		resource->Unmap(0, nullptr);
	} else {
		resource->Release();
		DX12LoadedResource failedResource;
		failedResource.failed = true;
		failedResource.name = name;
		failedResource.path = path;
		gLoadedResources[name] = failedResource;
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"map_failed\",\"hr\":\"0x%lx\"",
			config.section.c_str(), hr);
		wchar_t status[512];
		swprintf_s(status, L"DX12 mod error: failed to map resource\n%ls hr=0x%lx",
			config.section.c_str(), hr);
		DX12SetOverlayError(status);
		return nullptr;
	}

	DX12LoadedResource loadedResource;
	loadedResource.resource = resource;
	loadedResource.byteWidth = byteWidth;
	loadedResource.stride = config.hasStride ? config.stride : 0;
	loadedResource.format = ParseDx12ResourceFormat(config.format);
	loadedResource.name = name;
	loadedResource.path = path;
	auto inserted = gLoadedResources.emplace(name, loadedResource);
	DX12LogJsonFunc("DX12ResourceLoad",
		"\"status\":\"loaded\",\"section\":\"%S\",\"name\":\"%S\",\"path\":\"%S\",\"bytes\":%llu,\"stride\":%u,\"format\":%u",
		config.section.c_str(), name.c_str(), path.c_str(),
		static_cast<unsigned long long>(byteWidth), loadedResource.stride,
		static_cast<UINT>(loadedResource.format));
	return &inserted.first->second;
}

static DX12LoadedResource *EnsureLoadedResourceForPreSkin(
	ID3D12Device *device, const std::wstring &name)
{
	if (!device || name.empty())
		return nullptr;

	Bunny::ResourceConfig config;
	std::wstring baseDir;
	AcquireSRWLockExclusive(&gModLock);
	auto loaded = gLoadedResources.find(name);
	if (loaded != gLoadedResources.end()) {
		DX12LoadedResource *resource =
			loaded->second.resource && !loaded->second.failed ? &loaded->second : nullptr;
		ReleaseSRWLockExclusive(&gModLock);
		return resource;
	}
	auto configIt = gResources.find(name);
	if (configIt == gResources.end() || !ResourceConfigLooksLikeBuffer(configIt->second)) {
		ReleaseSRWLockExclusive(&gModLock);
		return nullptr;
	}
	config = configIt->second;
	baseDir = gBaseDir;
	ReleaseSRWLockExclusive(&gModLock);

	std::vector<unsigned char> bytes;
	std::wstring path;
	if (!LoadResourceBytes(config, baseDir, &bytes, &path)) {
		DX12LoadedResource failedResource;
		failedResource.failed = true;
		failedResource.name = name;
		failedResource.path = path;
		AcquireSRWLockExclusive(&gModLock);
		auto inserted = gLoadedResources.emplace(name, failedResource);
		DX12LoadedResource *result =
			inserted.first->second.resource && !inserted.first->second.failed ?
			&inserted.first->second : nullptr;
		ReleaseSRWLockExclusive(&gModLock);
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"read_failed\",\"path\":\"%S\"",
			config.section.c_str(), path.c_str());
		wchar_t status[512];
		swprintf_s(status, L"DX12 mod warning: failed to read resource\n%ls\n%ls",
			config.section.c_str(), path.c_str());
		DX12SetOverlayWarning(status);
		return result;
	}

	UINT64 byteWidth = config.hasByteWidth ? config.byteWidth : bytes.size();
	if (byteWidth < bytes.size())
		byteWidth = bytes.size();
	if (byteWidth == 0)
		return nullptr;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = byteWidth;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ID3D12Resource *resource = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr, IID_PPV_ARGS(&resource));
	if (FAILED(hr) || !resource) {
		DX12LoadedResource failedResource;
		failedResource.failed = true;
		failedResource.name = name;
		failedResource.path = path;
		AcquireSRWLockExclusive(&gModLock);
		gLoadedResources.emplace(name, failedResource);
		ReleaseSRWLockExclusive(&gModLock);
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"create_failed\",\"hr\":\"0x%lx\",\"bytes\":%llu",
			config.section.c_str(), hr, static_cast<unsigned long long>(byteWidth));
		wchar_t status[512];
		swprintf_s(status, L"DX12 mod error: failed to create resource\n%ls hr=0x%lx",
			config.section.c_str(), hr);
		DX12SetOverlayError(status);
		return nullptr;
	}

	void *mapped = nullptr;
	D3D12_RANGE readRange = {};
	hr = resource->Map(0, &readRange, &mapped);
	if (SUCCEEDED(hr) && mapped) {
		memcpy(mapped, bytes.data(), bytes.size());
		if (byteWidth > bytes.size())
			memset(static_cast<unsigned char*>(mapped) + bytes.size(), 0, byteWidth - bytes.size());
		resource->Unmap(0, nullptr);
	} else {
		resource->Release();
		DX12LoadedResource failedResource;
		failedResource.failed = true;
		failedResource.name = name;
		failedResource.path = path;
		AcquireSRWLockExclusive(&gModLock);
		gLoadedResources.emplace(name, failedResource);
		ReleaseSRWLockExclusive(&gModLock);
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"map_failed\",\"hr\":\"0x%lx\"",
			config.section.c_str(), hr);
		wchar_t status[512];
		swprintf_s(status, L"DX12 mod error: failed to map resource\n%ls hr=0x%lx",
			config.section.c_str(), hr);
		DX12SetOverlayError(status);
		return nullptr;
	}

	DX12LoadedResource loadedResource;
	loadedResource.resource = resource;
	loadedResource.byteWidth = byteWidth;
	loadedResource.stride = config.hasStride ? config.stride : 0;
	loadedResource.format = ParseDx12ResourceFormat(config.format);
	loadedResource.name = name;
	loadedResource.path = path;

	AcquireSRWLockExclusive(&gModLock);
	auto inserted = gLoadedResources.emplace(name, loadedResource);
	if (!inserted.second)
		resource->Release();
	DX12LoadedResource *result =
		inserted.first->second.resource && !inserted.first->second.failed ?
		&inserted.first->second : nullptr;
	ReleaseSRWLockExclusive(&gModLock);

	if (inserted.second) {
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"loaded\",\"section\":\"%S\",\"name\":\"%S\",\"path\":\"%S\",\"bytes\":%llu,\"stride\":%u,\"format\":%u",
			config.section.c_str(), name.c_str(), path.c_str(),
			static_cast<unsigned long long>(byteWidth), loadedResource.stride,
			static_cast<UINT>(loadedResource.format));
	}
	return result;
}

static bool EnsureResourceUavLocked(
	ID3D12Device *device, ID3D12GraphicsCommandList *commandList,
	DX12LoadedResource *resource, UINT elementStride, UINT64 minByteWidth = 0,
	bool allowInactiveExplicitMatchCs = false)
{
	if (!device || !commandList || !resource || !resource->resource)
		return false;
	if (!allowInactiveExplicitMatchCs &&
	    ResourceBlockedByInactiveExplicitMatchCsLocked(resource->name))
		return false;
	if (!elementStride)
		elementStride = resource->stride ? resource->stride : 4;
	if (!elementStride)
		return false;
	const UINT64 uavByteWidth = (std::max)(resource->byteWidth, minByteWidth);
	if (resource->uavHeap && resource->uavResource &&
	    resource->uavByteWidth >= uavByteWidth &&
	    resource->uavStride == elementStride)
		return true;

	if (resource->uavResource) {
		RetirePreSkinResourceForCommandList(commandList, resource->uavResource);
		resource->uavResource = nullptr;
	}
	if (resource->uavHeap) {
		RetirePreSkinDescriptorHeap(resource->uavHeap);
		resource->uavHeap = nullptr;
	}
	resource->uavCpu = {};
	resource->uavGpu = {};
	resource->uavInitialized = false;
	resource->uavWritten = false;
	resource->uavValid = false;
	resource->uavByteWidth = 0;
	resource->uavStride = 0;
	resource->uavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	heapDesc.NodeMask = 0;

	ID3D12DescriptorHeap *heap = nullptr;
	HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
	if (FAILED(hr) || !heap) {
		DX12LogDebugJsonFunc("DX12PreSkinningUav",
			"\"status\":\"failed\",\"reason\":\"create_heap_failed\",\"hr\":\"0x%lx\",\"resource\":\"%S\"",
			hr, resource->name.c_str());
		return false;
	}

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = uavByteWidth;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	ID3D12Resource *uavResource = nullptr;
	hr = device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr, IID_PPV_ARGS(&uavResource));
	if (FAILED(hr) || !uavResource) {
		DX12LogDebugJsonFunc("DX12PreSkinningUav",
			"\"status\":\"failed\",\"reason\":\"create_resource_failed\",\"hr\":\"0x%lx\",\"resource\":\"%S\",\"bytes\":%llu",
			hr, resource->name.c_str(), static_cast<unsigned long long>(uavByteWidth));
		heap->Release();
		return false;
	}

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.StructureByteStride = elementStride;
	uavDesc.Buffer.NumElements = static_cast<UINT>(
		(std::min)(uavByteWidth / elementStride, static_cast<UINT64>(UINT_MAX)));
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

	resource->uavCpu = heap->GetCPUDescriptorHandleForHeapStart();
	resource->uavGpu = heap->GetGPUDescriptorHandleForHeapStart();
	device->CreateUnorderedAccessView(uavResource, nullptr, &uavDesc, resource->uavCpu);
	resource->uavHeap = heap;
	resource->uavResource = uavResource;
	resource->uavByteWidth = uavByteWidth;
	resource->uavStride = elementStride;
	resource->uavInitialized = true;
	resource->uavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	DX12LogDebugJsonFunc("DX12PreSkinningUav",
		"\"status\":\"created\",\"resource\":\"%S\",\"bytes\":%llu,\"uavBytes\":%llu,\"stride\":%u,\"elements\":%u",
		resource->name.c_str(),
		static_cast<unsigned long long>(resource->byteWidth),
		static_cast<unsigned long long>(uavByteWidth),
		elementStride, uavDesc.Buffer.NumElements);
	return true;
}

static bool EnsureResourceSrvLocked(
	ID3D12Device *device, DX12LoadedResource *resource, UINT elementStride,
	UINT64 minByteWidth = 0)
{
	if (!device || !resource || !resource->resource)
		return false;
	if (!elementStride)
		elementStride = resource->stride ? resource->stride : 4;
	if (!elementStride)
		return false;
	const UINT64 srvByteWidth = (std::max)(resource->byteWidth, minByteWidth);
	if (resource->srvHeap && resource->srvCpu.ptr &&
	    resource->srvStride == elementStride && resource->srvByteWidth >= srvByteWidth)
		return true;

	if (resource->srvHeap) {
		resource->srvHeap->Release();
		resource->srvHeap = nullptr;
	}
	if (resource->srvResource) {
		resource->srvResource->Release();
		resource->srvResource = nullptr;
	}
	resource->srvCpu = {};
	resource->srvStride = 0;
	resource->srvByteWidth = 0;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	heapDesc.NodeMask = 0;

	ID3D12DescriptorHeap *heap = nullptr;
	HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
	if (FAILED(hr) || !heap) {
		DX12LogDebugJsonFunc("DX12PreSkinningSrv",
			"\"status\":\"failed\",\"reason\":\"create_heap_failed\",\"hr\":\"0x%lx\",\"resource\":\"%S\"",
			hr, resource->name.c_str());
		return false;
	}

	ID3D12Resource *srvBacking = resource->resource;
	if (srvByteWidth > resource->byteWidth) {
		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProps.CreationNodeMask = 1;
		heapProps.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = srvByteWidth;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		hr = device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr, IID_PPV_ARGS(&srvBacking));
		if (FAILED(hr) || !srvBacking) {
			heap->Release();
			DX12LogDebugJsonFunc("DX12PreSkinningSrv",
				"\"status\":\"failed\",\"reason\":\"create_padded_resource_failed\",\"hr\":\"0x%lx\",\"resource\":\"%S\",\"bytes\":%llu",
				hr, resource->name.c_str(), static_cast<unsigned long long>(srvByteWidth));
			return false;
		}

		void *dst = nullptr;
		D3D12_RANGE dstReadRange = {};
		hr = srvBacking->Map(0, &dstReadRange, &dst);
		if (FAILED(hr) || !dst) {
			srvBacking->Release();
			heap->Release();
			return false;
		}
		memset(dst, 0, static_cast<size_t>(srvByteWidth));
		void *src = nullptr;
		D3D12_RANGE srcReadRange = { 0, static_cast<SIZE_T>(resource->byteWidth) };
		hr = resource->resource->Map(0, &srcReadRange, &src);
		if (SUCCEEDED(hr) && src) {
			memcpy(dst, src, static_cast<size_t>(resource->byteWidth));
			D3D12_RANGE srcWrittenRange = {};
			resource->resource->Unmap(0, &srcWrittenRange);
		}
		srvBacking->Unmap(0, nullptr);
		if (FAILED(hr) || !src) {
			srvBacking->Release();
			heap->Release();
			DX12LogDebugJsonFunc("DX12PreSkinningSrv",
				"\"status\":\"failed\",\"reason\":\"copy_padded_resource_failed\",\"hr\":\"0x%lx\",\"resource\":\"%S\"",
				hr, resource->name.c_str());
			return false;
		}
		resource->srvResource = srvBacking;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = static_cast<UINT>(
		(std::min)(resource->byteWidth / elementStride, static_cast<UINT64>(UINT_MAX)));
	srvDesc.Buffer.StructureByteStride = elementStride;
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	resource->srvCpu = heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateShaderResourceView(srvBacking, &srvDesc, resource->srvCpu);
	resource->srvHeap = heap;
	resource->srvStride = elementStride;
	resource->srvByteWidth = srvByteWidth;
	DX12LogDebugJsonFunc("DX12PreSkinningSrv",
		"\"status\":\"created\",\"resource\":\"%S\",\"bytes\":%llu,\"srvBytes\":%llu,\"stride\":%u,\"elements\":%u",
		resource->name.c_str(),
		static_cast<unsigned long long>(resource->byteWidth),
		static_cast<unsigned long long>(srvByteWidth),
		elementStride, srvDesc.Buffer.NumElements);
	return true;
}

static DX12LoadedResource *FindReplacementResourceForVlrLocked(
	ID3D12Device *device, const DX12VertexLimitRaiseConfig &vlr)
{
	DX12LoadedResource *best = nullptr;
	for (const std::wstring &name : gVlrResourceCandidates) {
		auto configIt = gResources.find(name);
		if (configIt == gResources.end())
			continue;
		const Bunny::ResourceConfig &config = configIt->second;
		if (config.hasStride && vlr.overrideByteStride &&
		    config.stride != vlr.overrideByteStride)
			continue;
		DX12LoadedResource *resource = EnsureLoadedResourceLocked(device, name);
		if (!resource || !resource->resource)
			continue;
		if (resource->byteWidth < vlr.overrideByteWidth)
			continue;
		if (!best || resource->byteWidth < best->byteWidth)
			best = resource;
	}
	return best;
}

static bool FindReplacementResourceNameForVlrLocked(
	const DX12VertexLimitRaiseConfig &vlr, std::wstring *resourceName)
{
	if (resourceName)
		resourceName->clear();
	for (const std::wstring &name : gVlrResourceCandidates) {
		auto configIt = gResources.find(name);
		if (configIt == gResources.end())
			continue;
		const Bunny::ResourceConfig &config = configIt->second;
		if (config.hasStride && vlr.overrideByteStride &&
		    config.stride != vlr.overrideByteStride)
			continue;
		if (resourceName)
			*resourceName = name;
		return true;
	}
	return false;
}

static bool ProducerMatchesAnyVlr(
	const DX12ComputeUavProducer &producer, uint64_t computeShaderHash,
	DX12VertexLimitRaiseConfig *match)
{
	for (const DX12VertexLimitRaiseConfig &vlr : gVertexLimitRaiseConfigs) {
		if (vlr.hasMatchCs && vlr.matchCs != computeShaderHash)
			continue;
		if (!ProducerMatchesReplacementTarget(producer, vlr))
			continue;
		if (match)
			*match = vlr;
		return true;
	}
	return false;
}

static bool ResourceMatchesVlrTarget(
	const DX12LoadedResource &resource, const DX12VertexLimitRaiseConfig &vlr)
{
	if (!vlr.overrideByteWidth)
		return false;
	if (resource.byteWidth < vlr.overrideByteWidth)
		return false;
	if (vlr.overrideByteStride && resource.stride &&
	    resource.stride != vlr.overrideByteStride)
		return false;
	return true;
}
