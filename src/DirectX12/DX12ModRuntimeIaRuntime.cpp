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
static bool TextureOverrideRuntimeEffectLocked(
	const Bunny::TextureOverrideConfig &config, bool includePost);
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
	for (auto &item : gLoadedResources)
		item.second.uavWritten = false;
	gPreSkinSectionAppliedPresent.clear();
	AcquireSRWLockExclusive(&gPreSkinLock);
	ClearActivePreSkinTextureOverridesLocked();
	ReleaseSRWLockExclusive(&gPreSkinLock);
	gPreSkinCbvReadCache.clear();
	gPreSkinSrvNegativeCache.clear();
	gPreSkinSrvPositiveCache.clear();
	gIaReplacementPreparedFrameCache.clear();
	gIaReplacementPrepareCachePresent = DX12GetPresentCount();
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

static bool TextureOverrideHasSkipLocked(
	uint32_t hash, uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	std::wstring *section, bool indexBuffer, uint32_t vertexSlot)
{
	const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
		hash, vertexCount, indexCount, instanceCount, 0, 0, 0, true, indexBuffer, vertexSlot);
	if (!config)
		return false;
	if (section)
		*section = config->section;
	return true;
}

static const Bunny::TextureOverrideConfig *FindMatchingIaOverrideLocked(
	uint32_t ibHash, const uint32_t *vbHashes, size_t vbHashCount,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount)
{
	if (ibHash) {
		const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
			ibHash, vertexCount, indexCount, instanceCount, 0, 0, 0, false, true, 0);
		if (config)
			return config;
	}

	if (!vbHashes)
		return nullptr;
	for (size_t i = 0; i < vbHashCount; ++i) {
		if (!vbHashes[i])
			continue;
		const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
			vbHashes[i], vertexCount, indexCount, instanceCount, 0, 0, 0, false, false,
			static_cast<uint32_t>(i));
		if (config)
			return config;
	}
	return nullptr;
}

static const Bunny::TextureOverrideConfig *FindMatchingIaOverrideLocked(
	const DX12IaHashState &iaState,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex, uint32_t firstInstance)
{
	if (iaState.hasIndexBuffer && iaState.indexHash) {
		const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
			iaState.indexHash, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance, false, true, 0);
		if (config)
			return config;
	}

	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		if (!buffer.hash)
			continue;
		const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
			buffer.hash, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance, false, false, buffer.slot);
		if (!config)
			continue;
		return config;
	}
	return nullptr;
}

static void AppendTextureOverridesForHashLocked(
	uint32_t hash, uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex, uint32_t firstInstance,
	std::vector<DX12TriggeredTextureOverride> *overrides, std::unordered_set<uint32_t> *seen,
	std::unordered_set<std::wstring> *fallbackSeen,
	bool indexBuffer, uint32_t vertexSlot, bool executeCommands)
{
	if (!overrides || !seen || !fallbackSeen)
		return;

	const std::vector<size_t> *candidateIndexes =
		FindIaTextureOverrideCandidatesLocked(hash, indexBuffer, vertexSlot);
	if (!candidateIndexes || candidateIndexes->empty())
		return;

	for (size_t index : *candidateIndexes) {
		if (index >= gIaTextureOverrides.size())
			continue;
		const DX12IaTextureOverrideCandidate &candidate = gIaTextureOverrides[index];
		const Bunny::TextureOverrideConfig &config = candidate.config;
		if (!TextureOverrideMatchCsSatisfiedLocked(config))
			continue;
		if (!TextureOverrideMatchesIaBinding(config, indexBuffer, vertexSlot))
			continue;
		if (!TextureOverrideMatchesDrawContext(
			config, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance))
			continue;
		if (!indexBuffer && indexCount != 0 && !executeCommands &&
		    config.indexBufferResource.empty() &&
		    !TextureOverrideHasDrawContextMatch(config) &&
		    !TextureOverrideHasDrawKind(config, Bunny::CommandListActionKind::Draw) &&
		    !TextureOverrideHasDrawKind(config, Bunny::CommandListActionKind::DrawIndexed) &&
		    !TextureOverrideHasDrawKind(config, Bunny::CommandListActionKind::DrawFromCaller) &&
		    !TextureOverrideHasDrawKind(config, Bunny::CommandListActionKind::DrawIndexedFromCaller)) {
			continue;
		}
		if (!executeCommands && !candidate.preRuntimeEffect)
			continue;
		if (candidate.sectionId) {
			if (!seen->insert(candidate.sectionId).second)
				continue;
		} else {
			const bool inserted = fallbackSeen->insert(config.section).second;
			LogIaFallbackPathLimited("ia_section_id_missing", config.section, inserted);
			if (!inserted)
				continue;
		}
		DX12TriggeredTextureOverride entry;
		entry.config = &candidate.config;
		entry.sectionId = candidate.sectionId;
		entry.postRuntimeEffect = candidate.postRuntimeEffect;
		entry.indexBuffer = indexBuffer;
		entry.vertexSlot = vertexSlot;
		entry.executeCommands = executeCommands;
		entry.executeDrawActions = executeCommands;
		entry.directIaAnchor = indexBuffer || executeCommands ||
			TextureOverrideHasDrawContextMatch(config) ||
			!config.indexBufferResource.empty();
		overrides->push_back(entry);
	}
}

static bool ActivePreSkinProducerMatchesIaState(
	const DX12ActivePreSkinTextureOverride &active,
	const DX12IaHashState &iaState,
	uint32_t *matchedSlot)
{
	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		if (!ProducerMatchesIaBuffer(active.producer, buffer))
			continue;
		if (matchedSlot)
			*matchedSlot = buffer.slot;
		return true;
	}
	return false;
}

static void AppendActivePreSkinTextureOverridesLocked(
	const DX12IaHashState &iaState,
	std::vector<DX12TriggeredTextureOverride> *overrides,
	std::unordered_set<uint32_t> *seen,
	std::unordered_set<std::wstring> *fallbackSeen,
	bool executeCommands)
{
	if (!overrides || !seen || !fallbackSeen)
		return;

	std::vector<DX12ActivePreSkinTextureOverride> activeOverrides;
	AcquireSRWLockShared(&gPreSkinLock);
	activeOverrides.reserve(gActivePreSkinTextureOverrides.size());
	for (const auto &item : gActivePreSkinTextureOverrides) {
		activeOverrides.push_back(item.second);
		if (activeOverrides.back().outputResource)
			activeOverrides.back().outputResource->AddRef();
	}
	ReleaseSRWLockShared(&gPreSkinLock);
	if (activeOverrides.empty())
		return;

	for (const DX12ActivePreSkinTextureOverride &active : activeOverrides) {
		const Bunny::TextureOverrideConfig &config = active.config;
		uint32_t matchedSlot = UINT_MAX;
		if (!ActivePreSkinProducerMatchesIaState(active, iaState, &matchedSlot))
			continue;
		if (!executeCommands && !TextureOverrideRuntimeEffectLocked(config, false))
			continue;

		auto sectionIdIt = gTextureOverrideSectionIds.find(config.section);
		const uint32_t sectionId =
			sectionIdIt != gTextureOverrideSectionIds.end() ? sectionIdIt->second : 0;
		if (sectionId) {
			if (!seen->insert(sectionId).second)
				continue;
		} else {
			const bool inserted = fallbackSeen->insert(config.section).second;
			LogIaFallbackPathLimited("preskin_section_id_missing", config.section, inserted);
			if (!inserted)
				continue;
		}

		DX12TriggeredTextureOverride entry;
		entry.fallbackConfig = config;
		entry.sectionId = sectionId;
		entry.postRuntimeEffect = TextureOverrideRuntimeEffectLocked(config, true);
		entry.indexBuffer = false;
		entry.vertexSlot = matchedSlot;
		entry.executeCommands = false;
		entry.executeDrawActions = false;
		entry.directIaAnchor = true;
		entry.preSkinAnchor = true;
		entry.preSkinResource = active.outputResource;
		entry.preSkinByteWidth = active.outputByteWidth;
		entry.preSkinStride = active.outputStride;
		entry.preSkinDescriptor = active.producer.descriptor;
		entry.preSkinHasDescriptor = active.producer.hasDescriptor;
		overrides->push_back(entry);
	}
	for (DX12ActivePreSkinTextureOverride &active : activeOverrides)
		ReleaseActivePreSkinTextureOverride(&active);
}
static void AppendRelatedMeshTextureOverridesLocked(
	std::vector<DX12TriggeredTextureOverride> *overrides,
	std::unordered_set<uint32_t> *seen,
	std::unordered_set<std::wstring> *fallbackSeen);

static void FindMatchingIaOverridesLocked(
	const DX12IaHashState &iaState,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex, uint32_t firstInstance,
	std::vector<DX12TriggeredTextureOverride> *overrides)
{
	if (!overrides)
		return;
	overrides->clear();
	std::unordered_set<uint32_t> seen;
	std::unordered_set<std::wstring> fallbackSeen;
	const bool indexedCaller = indexCount != 0;
	if (iaState.hasIndexBuffer)
		AppendTextureOverridesForHashLocked(
			iaState.indexHash, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance, overrides, &seen, &fallbackSeen,
			true, 0, indexedCaller);
	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		AppendTextureOverridesForHashLocked(
			buffer.hash, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance, overrides, &seen, &fallbackSeen,
			false, buffer.slot, !indexedCaller);
	}
	if (iaState.hasIndexBuffer)
		AppendTextureOverridesForHashLocked(
			0, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance, overrides, &seen, &fallbackSeen,
			true, 0, indexedCaller);
	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		AppendTextureOverridesForHashLocked(
			0, vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, firstInstance, overrides, &seen,
			&fallbackSeen, false, buffer.slot, !indexedCaller);
	}
	AppendActivePreSkinTextureOverridesLocked(
		iaState, overrides, &seen, &fallbackSeen, !indexedCaller);
	AppendRelatedMeshTextureOverridesLocked(overrides, &seen, &fallbackSeen);

}

bool DX12ModIaMayHaveTextureOverrideMatch(const DX12IaHashState &iaState, bool indexedCaller)
{
	if (gHasTextureOverrides == 0)
		return false;


	uint64_t cacheKey = 14695981039346656037ull;
	cacheKey = HashCombine64(cacheKey, gReloadGeneration);
	cacheKey = HashCombine64(cacheKey, gPreSkinActiveGeneration);
	cacheKey = HashCombine64(cacheKey, indexedCaller ? 1 : 0);
	cacheKey = HashCombine64(cacheKey, iaState.hasIndexBuffer ? iaState.indexHash : 0);
	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		if (!buffer.hash)
			continue;
		cacheKey = HashCombine64(cacheKey, buffer.slot);
		cacheKey = HashCombine64(cacheKey, buffer.hash);
	}

	AcquireSRWLockShared(&gModLock);
	auto cached = gIaTextureCandidateCache.find(cacheKey);
	if (cached != gIaTextureCandidateCache.end()) {
		const bool mayMatch = cached->second;
		ReleaseSRWLockShared(&gModLock);
		return mayMatch;
	}
	ReleaseSRWLockShared(&gModLock);

	AcquireSRWLockShared(&gModLock);
	bool mayMatch = false;
	if (iaState.hasIndexBuffer) {
		mayMatch =
			FindIaTextureOverrideCandidatesLocked(iaState.indexHash, true, 0) != nullptr ||
			FindIaTextureOverrideCandidatesLocked(0, true, 0) != nullptr;
	}
	if (!mayMatch) {
		for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
			if (FindIaTextureOverrideCandidatesLocked(buffer.hash, false, buffer.slot) ||
			    FindIaTextureOverrideCandidatesLocked(0, false, buffer.slot)) {
				mayMatch = true;
				break;
			}
		}
	}
	ReleaseSRWLockShared(&gModLock);

	if (!mayMatch && DX12ModHasActivePreSkinTextureOverrides())
		mayMatch = true;

	AcquireSRWLockExclusive(&gModLock);
	if (gIaTextureCandidateCache.size() > 4096)
		gIaTextureCandidateCache.clear();
	gIaTextureCandidateCache[cacheKey] = mayMatch;
	ReleaseSRWLockExclusive(&gModLock);
	return mayMatch;
}

static const DX12IaBufferHash *FindIaVertexSlot(const DX12IaHashState &iaState, uint32_t slot)
{
	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		if (buffer.slot == slot)
			return &buffer;
	}
	return nullptr;
}

static void AppendTokenFromText(
	const std::wstring &text, std::set<std::wstring> *tokens)
{
	if (!tokens)
		return;
	std::wstring token;
	for (wchar_t ch : text) {
		const bool hex =
			(ch >= L'0' && ch <= L'9') ||
			(ch >= L'a' && ch <= L'f') ||
			(ch >= L'A' && ch <= L'F');
		if (hex) {
			token.push_back(static_cast<wchar_t>(towlower(ch)));
			continue;
		}
		if (token.length() >= 8)
			tokens->insert(token);
		token.clear();
	}
	if (token.length() >= 8)
		tokens->insert(token);
}

static void CollectTextureOverrideTokens(
	const Bunny::TextureOverrideConfig &config, std::set<std::wstring> *tokens)
{
	AppendTokenFromText(config.section, tokens);
	AppendTokenFromText(config.originalSection, tokens);
	AppendTokenFromText(config.indexBufferResource, tokens);
	for (const auto &item : config.vertexBufferResources)
		AppendTokenFromText(item.second, tokens);
}

static bool TextureOverrideSharesAnyToken(
	const std::set<std::wstring> &candidateTokens, const std::set<std::wstring> &tokens)
{
	if (candidateTokens.empty() || tokens.empty())
		return false;
	for (const std::wstring &token : candidateTokens) {
		if (tokens.find(token) != tokens.end())
			return true;
	}
	return false;
}

static void BuildDx12RelatedTextureOverrideIndex(
	const Bunny::TextureOverrideMap &textureOverrides,
	const std::unordered_map<std::wstring, bool> &textureOverridePreEffect,
	const std::unordered_map<std::wstring, uint32_t> &textureOverrideSectionIds,
	std::vector<DX12RelatedTextureOverrideCandidate> *relatedTextureOverrides,
	std::unordered_map<std::wstring, std::vector<size_t>> *relatedTokenIndex)
{
	if (!relatedTextureOverrides || !relatedTokenIndex)
		return;

	relatedTextureOverrides->clear();
	relatedTokenIndex->clear();
	for (const auto &bucket : textureOverrides) {
		for (const Bunny::TextureOverrideConfig &config : bucket.second) {
			auto effect = textureOverridePreEffect.find(config.section);
			if (effect == textureOverridePreEffect.end() || !effect->second)
				continue;

			DX12RelatedTextureOverrideCandidate candidate;
			candidate.config = config;
			auto sectionId = textureOverrideSectionIds.find(config.section);
			candidate.sectionId =
				sectionId != textureOverrideSectionIds.end() ? sectionId->second : 0;
			CollectTextureOverrideTokens(candidate.config, &candidate.tokens);
			if (candidate.tokens.empty())
				continue;

			const size_t index = relatedTextureOverrides->size();
			relatedTextureOverrides->push_back(std::move(candidate));
			for (const std::wstring &token : (*relatedTextureOverrides)[index].tokens)
				(*relatedTokenIndex)[token].push_back(index);
		}
	}
}

static void AppendRelatedMeshTextureOverridesLocked(
	std::vector<DX12TriggeredTextureOverride> *overrides,
	std::unordered_set<uint32_t> *seen,
	std::unordered_set<std::wstring> *fallbackSeen)
{
	if (!overrides || !seen || !fallbackSeen || overrides->empty())
		return;

	bool hasDirectIndexAnchor = false;
	bool hasPreSkinAnchor = false;
	bool hasVerifiedResourceAnchor = false;
	std::unordered_set<std::wstring> namespaces;
	std::set<std::wstring> tokens;
	for (const DX12TriggeredTextureOverride &entry : *overrides) {
		if (!entry.directIaAnchor && !entry.preSkinAnchor)
			continue;
		const Bunny::TextureOverrideConfig &config =
			TriggeredTextureOverrideConfig(entry);
		if (entry.indexBuffer)
			hasDirectIndexAnchor = true;
		if (entry.preSkinAnchor) {
			hasPreSkinAnchor = true;
			hasVerifiedResourceAnchor = true;
		}
		if (entry.directIaAnchor && (entry.indexBuffer || entry.preSkinResource))
			hasVerifiedResourceAnchor = true;
		if (!config.iniNamespace.empty())
			namespaces.insert(config.iniNamespace);
		CollectTextureOverrideTokens(config, &tokens);
	}
	if ((!hasDirectIndexAnchor && !hasPreSkinAnchor && !hasVerifiedResourceAnchor) ||
	    tokens.empty())
		return;

	std::vector<const std::vector<size_t>*> indexedLists;
	std::vector<size_t> indexedPositions;
	indexedLists.reserve(tokens.size());
	indexedPositions.reserve(tokens.size());
	for (const std::wstring &token : tokens) {
		auto indexed = gRelatedTextureOverrideTokenIndex.find(token);
		if (indexed == gRelatedTextureOverrideTokenIndex.end())
			continue;
		if (indexed->second.empty())
			continue;
		indexedLists.push_back(&indexed->second);
		indexedPositions.push_back(0);
	}

	while (!indexedLists.empty()) {
		size_t index = SIZE_MAX;
		for (size_t listIndex = 0; listIndex < indexedLists.size(); ++listIndex) {
			const std::vector<size_t> &list = *indexedLists[listIndex];
			const size_t position = indexedPositions[listIndex];
			if (position < list.size() && list[position] < index)
				index = list[position];
		}
		if (index == SIZE_MAX)
			break;
		for (size_t listIndex = 0; listIndex < indexedLists.size(); ++listIndex) {
			const std::vector<size_t> &list = *indexedLists[listIndex];
			size_t &position = indexedPositions[listIndex];
			while (position < list.size() && list[position] <= index)
				++position;
		}
		if (index >= gRelatedTextureOverrides.size())
			continue;
		const DX12RelatedTextureOverrideCandidate &candidate =
			gRelatedTextureOverrides[index];
		const Bunny::TextureOverrideConfig &config = candidate.config;
		if (!TextureOverrideMatchCsSatisfiedLocked(config))
			continue;
		if (!namespaces.empty() &&
		    namespaces.find(config.iniNamespace) == namespaces.end())
			continue;
		if (!TextureOverrideSharesAnyToken(candidate.tokens, tokens))
			continue;
		if (candidate.sectionId) {
			if (!seen->insert(candidate.sectionId).second)
				continue;
		} else {
			const bool inserted = fallbackSeen->insert(config.section).second;
			LogIaFallbackPathLimited("related_section_id_missing", config.section, inserted);
			if (!inserted)
				continue;
		}
		DX12TriggeredTextureOverride entry;
		entry.config = &candidate.config;
		entry.sectionId = candidate.sectionId;
		entry.postRuntimeEffect = true;
		entry.indexBuffer = !config.indexBufferResource.empty() ||
			config.vertexBufferResources.empty();
		entry.vertexSlot = config.vertexBufferResources.empty() ?
			0 : config.vertexBufferResources.begin()->first;
		entry.executeCommands = false;
		entry.executeDrawActions = hasPreSkinAnchor;
		entry.relatedMesh = true;
		overrides->push_back(entry);
	}
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

static bool AppendPreSkinResourceViewLocked(
	ID3D12GraphicsCommandList *commandList, const DX12IaHashState &iaState,
	const DX12TriggeredTextureOverride &entry, DX12ModIaReplacement *replacement)
{
	if (!commandList || !replacement || !entry.preSkinResource)
		return false;
	if (entry.preSkinByteWidth == 0 || entry.preSkinStride == 0)
		return false;

	D3D12_RESOURCE_STATES preparedBeforeState = D3D12_RESOURCE_STATE_COMMON;
	bool preparedTransition = false;
	bool preparedUavBarrier = false;
	if (entry.preSkinHasDescriptor &&
	    entry.preSkinDescriptor.kind == "UAV" &&
	    entry.preSkinDescriptor.resource == entry.preSkinResource) {
		DX12ActivePreSkinOutputState outputState = {};
		auto stateIt = gActivePreSkinOutputStates.find(entry.preSkinResource);
		if (stateIt != gActivePreSkinOutputStates.end())
			outputState = stateIt->second;
		else {
			outputState.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			outputState.uavBarrierPending = true;
		}
		D3D12_RESOURCE_STATES beforeState = outputState.state;
		DX12BufferResourceSummary tracked = {};
		if (DX12ResolveBufferResourceByGpuVa(
			entry.preSkinResource->GetGPUVirtualAddress(), entry.preSkinByteWidth, &tracked) &&
		    tracked.hasCurrentState &&
		    tracked.resource == entry.preSkinResource) {
			beforeState = static_cast<D3D12_RESOURCE_STATES>(tracked.currentState);
			outputState.state = beforeState;
		}
		if (outputState.uavBarrierPending &&
		    beforeState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
			D3D12_RESOURCE_BARRIER uavBarrier = {};
			uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			uavBarrier.UAV.pResource = entry.preSkinResource;
			commandList->ResourceBarrier(1, &uavBarrier);
			preparedUavBarrier = true;
		}
		outputState.uavBarrierPending = false;
		if (beforeState != D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) {
			D3D12_RESOURCE_BARRIER transition = {};
			transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			transition.Transition.pResource = entry.preSkinResource;
			transition.Transition.StateBefore = beforeState;
			transition.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commandList->ResourceBarrier(1, &transition);
			outputState.state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			preparedTransition = true;
		}
		gActivePreSkinOutputStates[entry.preSkinResource] = outputState;
		preparedBeforeState = beforeState;
#if defined(_DEBUG)
		DX12LogDebugJsonFunc("DX12PreSkinIaResourcePrepare",
			"\"slot\":%u,\"bytes\":%llu,\"stride\":%u,\"beforeState\":%u,\"uavBarrier\":%s,\"transition\":%s,\"gpuVa\":\"0x%llx\"",
			entry.vertexSlot,
			static_cast<unsigned long long>(entry.preSkinByteWidth),
			entry.preSkinStride,
			static_cast<UINT>(preparedBeforeState),
			preparedUavBarrier ? "true" : "false",
			preparedTransition ? "true" : "false",
			static_cast<unsigned long long>(entry.preSkinResource->GetGPUVirtualAddress()));
#endif
	}

	if (replacement->vertexBuffers.empty() ||
	    entry.vertexSlot < replacement->vertexBufferStartSlot ||
	    entry.vertexSlot >= replacement->vertexBufferStartSlot + replacement->vertexBuffers.size()) {
		uint32_t minSlot = replacement->vertexBuffers.empty() ?
			entry.vertexSlot : (std::min)(replacement->vertexBufferStartSlot, entry.vertexSlot);
		uint32_t maxSlot = replacement->vertexBuffers.empty() ?
			entry.vertexSlot : (std::max)(
				replacement->vertexBufferStartSlot +
				static_cast<uint32_t>(replacement->vertexBuffers.size()) - 1,
				entry.vertexSlot);
		if (maxSlot - minSlot >= 32)
			return false;
		std::vector<D3D12_VERTEX_BUFFER_VIEW> resized(maxSlot - minSlot + 1);
		for (size_t i = 0; i < replacement->vertexBuffers.size(); ++i) {
			uint32_t slot = replacement->vertexBufferStartSlot + static_cast<uint32_t>(i);
			resized[slot - minSlot] = replacement->vertexBuffers[i];
		}
		replacement->vertexBufferStartSlot = minSlot;
		replacement->vertexBuffers.swap(resized);
	}

	D3D12_VERTEX_BUFFER_VIEW &view =
		replacement->vertexBuffers[entry.vertexSlot - replacement->vertexBufferStartSlot];
	view.BufferLocation = entry.preSkinResource->GetGPUVirtualAddress();
	view.SizeInBytes = static_cast<UINT>((std::min)(
		entry.preSkinByteWidth, static_cast<UINT64>(UINT_MAX)));
	const DX12IaBufferHash *sourceSlot = FindIaVertexSlot(iaState, entry.vertexSlot);
	view.StrideInBytes = entry.preSkinStride ? entry.preSkinStride :
		(sourceSlot ? sourceSlot->vertexView.StrideInBytes : 0);
	replacement->RetainResource(entry.preSkinResource);
	if (view.BufferLocation != 0 && view.SizeInBytes != 0 && view.StrideInBytes != 0) {
		RetainPreSkinResourceForCommandList(commandList, entry.preSkinResource);
		return true;
	}
	return false;
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

	return nullptr;
}

