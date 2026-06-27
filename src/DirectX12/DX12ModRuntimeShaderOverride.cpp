static void AppendShaderOverrideForHashLocked(
	uint64_t hash, std::vector<const Bunny::ShaderOverrideConfig*> *configs,
	std::unordered_set<std::wstring> *seen)
{
	if (!hash || !configs || !seen)
		return;
	auto it = gShaderOverrides.find(hash);
	if (it == gShaderOverrides.end())
		return;
	if (!seen->insert(it->second.section).second)
		return;
	configs->push_back(&it->second);
}

static void FindShaderOverridesForPsoLocked(
	ID3D12PipelineState *pipelineState, bool dispatch,
	std::vector<const Bunny::ShaderOverrideConfig*> *configs)
{
	if (!configs)
		return;
	configs->clear();
	if (!pipelineState)
		return;

	auto cached = gShaderOverridePsoMatchCache.find(pipelineState);
	if (cached != gShaderOverridePsoMatchCache.end() &&
	    cached->second.generation == gReloadGeneration) {
		*configs = dispatch ? cached->second.dispatchConfigs : cached->second.drawConfigs;
#if defined(_DEBUG)
			DX12LogDebugJsonFunc("DX12ShaderOverridePsoMatchCache",
				"\"status\":\"hit\",\"pso\":\"%p\",\"dispatch\":%s,\"matches\":%zu",
				pipelineState, dispatch ? "true" : "false", configs->size());
#endif
		return;
	}

	DX12PsoShaderInfo info = {};
	if (!DX12GetPipelineStateShaderInfo(pipelineState, &info))
		return;

	std::unordered_set<std::wstring> seen;
	DX12ShaderOverridePsoMatchCache matchCache;
	matchCache.generation = gReloadGeneration;
	if (dispatch) {
		if (info.hasCS)
			AppendShaderOverrideForHashLocked(info.cs, &matchCache.dispatchConfigs, &seen);
		*configs = matchCache.dispatchConfigs;
		gShaderOverridePsoMatchCache[pipelineState] = std::move(matchCache);
		return;
	}

	if (info.hasVS)
		AppendShaderOverrideForHashLocked(info.vs, &matchCache.drawConfigs, &seen);
	if (info.hasPS)
		AppendShaderOverrideForHashLocked(info.ps, &matchCache.drawConfigs, &seen);
	*configs = matchCache.drawConfigs;
	gShaderOverridePsoMatchCache[pipelineState] = std::move(matchCache);
}

bool DX12ModPrepareShaderOverrideReplacement(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState,
	const DX12IaHashState &iaState,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex,
	DX12ModIaReplacement *replacement)
{
	if (!replacement)
		return false;
	*replacement = DX12ModIaReplacement();
	if (!commandList || !pipelineState || !DX12ModHasActiveShaderOverrides())
		return false;

	ID3D12Device *device = AcquireModDevice(commandList);
	if (!device)
		return false;

	std::vector<const Bunny::ShaderOverrideConfig*> configs;
	AcquireSRWLockShared(&gModLock);
	FindShaderOverridesForPsoLocked(pipelineState, false, &configs);
	ReleaseSRWLockShared(&gModLock);

	if (configs.empty()) {
		device->Release();
		return false;
	}

	AcquireSRWLockExclusive(&gModLock);
	DX12CommandListExecutor executor(
		device, commandList, iaState,
		vertexCount, indexCount, instanceCount,
		firstVertex, firstIndex, replacement);
	executor.SetExecutionMode(true, true);
	for (const Bunny::ShaderOverrideConfig *config : configs) {
		if (!config)
			continue;
		executor.RunShaderOverride(*config);
	}
	LogShaderOverrideCommandListLimited("pre", configs, *replacement);
	ReleaseSRWLockExclusive(&gModLock);
	device->Release();

	return executor.Changed() || replacement->skip || !replacement->draws.empty() ||
		!replacement->dispatches.empty();
}

void DX12ModRunPostShaderOverrideReplacement(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState,
	const DX12IaHashState &iaState,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstIndex,
	DX12ModIaReplacement *replacement)
{
	if (!replacement || !commandList || !pipelineState || !DX12ModHasActiveShaderOverrides())
		return;

	ID3D12Device *device = AcquireModDevice(commandList);
	if (!device)
		return;

	std::vector<const Bunny::ShaderOverrideConfig*> configs;
	AcquireSRWLockShared(&gModLock);
	FindShaderOverridesForPsoLocked(pipelineState, false, &configs);
	ReleaseSRWLockShared(&gModLock);

	if (!configs.empty()) {
		AcquireSRWLockExclusive(&gModLock);
		DX12CommandListExecutor executor(
			device, commandList, iaState,
			vertexCount, indexCount, instanceCount,
			firstVertex, firstIndex, replacement);
		executor.SetExecutionMode(true, true);
		for (const Bunny::ShaderOverrideConfig *config : configs) {
			if (!config)
				continue;
			executor.RunPostShaderOverrideLists(*config);
		}
		LogShaderOverrideCommandListLimited("post", configs, *replacement);
		ReleaseSRWLockExclusive(&gModLock);
	}
	device->Release();
}

static bool ShaderOverrideHasSkipLocked(uint64_t hash)
{
	auto it = gShaderOverrides.find(hash);
	return it != gShaderOverrides.end() && it->second.handlingSkip;
}

static bool ShaderBytecodeHasSkipLocked(const D3D12_SHADER_BYTECODE &bytecode)
{
	if (!bytecode.pShaderBytecode || !bytecode.BytecodeLength)
		return false;
	return ShaderOverrideHasSkipLocked(
		DX12ModHashShaderBytecode(bytecode.pShaderBytecode, bytecode.BytecodeLength));
}

static bool StoredPsoHasSkipLocked(const DX12StoredPso &record, bool dispatch)
{
	if (dispatch) {
		return record.kind == DX12PsoKind::Compute &&
			ShaderBytecodeHasSkipLocked(record.computeDesc.CS);
	}

	return record.kind == DX12PsoKind::Graphics &&
		(ShaderBytecodeHasSkipLocked(record.graphicsDesc.VS) ||
		 ShaderBytecodeHasSkipLocked(record.graphicsDesc.PS));
}

static void UpdateStoredPsoSkipLocked(DX12StoredPso *record)
{
	if (!record || record->skipGeneration == gReloadGeneration)
		return;

	record->skipDraw = false;
	record->skipDispatch = false;
	if (record->kind == DX12PsoKind::Compute) {
		record->skipDispatch = ShaderBytecodeHasSkipLocked(record->computeDesc.CS);
	} else {
		record->skipDraw =
			ShaderBytecodeHasSkipLocked(record->graphicsDesc.VS) ||
			ShaderBytecodeHasSkipLocked(record->graphicsDesc.PS);
	}
	record->skipGeneration = gReloadGeneration;
}

bool DX12ModShouldSkipPipelineState(ID3D12PipelineState *pipelineState, bool dispatch)
{
	if (!pipelineState || !DX12ModHasActiveShaderOverrides())
		return false;

	bool skip = false;
	AcquireSRWLockShared(&gModLock);
	auto record = gPsoRecords.find(pipelineState);
	if (record != gPsoRecords.end()) {
		if (record->second.skipGeneration == gReloadGeneration) {
			skip = dispatch ? record->second.skipDispatch : record->second.skipDraw;
			ReleaseSRWLockShared(&gModLock);
			return skip;
		}
	}
	ReleaseSRWLockShared(&gModLock);

	AcquireSRWLockExclusive(&gModLock);
	record = gPsoRecords.find(pipelineState);
	if (record != gPsoRecords.end()) {
		UpdateStoredPsoSkipLocked(&record->second);
		skip = dispatch ? record->second.skipDispatch : record->second.skipDraw;
		ReleaseSRWLockExclusive(&gModLock);
		return skip;
	}
	ReleaseSRWLockExclusive(&gModLock);

	DX12PsoShaderInfo info = {};
	if (!DX12GetPipelineStateShaderInfo(pipelineState, &info))
		return false;

	AcquireSRWLockShared(&gModLock);
	if (dispatch) {
		skip = info.hasCS && ShaderOverrideHasSkipLocked(info.cs);
	} else {
		skip = (info.hasVS && ShaderOverrideHasSkipLocked(info.vs)) ||
			(info.hasPS && ShaderOverrideHasSkipLocked(info.ps));
	}
	ReleaseSRWLockShared(&gModLock);
	return skip;
}

UINT64 DX12ModGetReloadGeneration()
{
	AcquireSRWLockShared(&gModLock);
	UINT64 generation = gReloadGeneration;
	ReleaseSRWLockShared(&gModLock);
	return generation;
}

void DX12ModRecordGraphicsPipelineState(
	ID3D12Device *device, ID3D12PipelineState *pipelineState,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
	if (!device || !pipelineState || !desc)
		return;

	DX12StoredPso record;
	record.kind = DX12PsoKind::Graphics;
	record.device = device;
	record.device->AddRef();
	DeepCopyGraphicsDesc(desc, &record);

	AcquireSRWLockExclusive(&gModLock);
	auto existing = gPsoRecords.find(pipelineState);
	if (existing != gPsoRecords.end())
		ReleaseStoredPso(&existing->second);
	gPsoRecords[pipelineState] = record;
	ReleaseSRWLockExclusive(&gModLock);
}

void DX12ModRecordComputePipelineState(
	ID3D12Device *device, ID3D12PipelineState *pipelineState,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc)
{
	if (!device || !pipelineState || !desc)
		return;

	DX12StoredPso record;
	record.kind = DX12PsoKind::Compute;
	record.device = device;
	record.device->AddRef();
	DeepCopyComputeDesc(desc, &record);

	AcquireSRWLockExclusive(&gModLock);
	auto existing = gPsoRecords.find(pipelineState);
	if (existing != gPsoRecords.end())
		ReleaseStoredPso(&existing->second);
	gPsoRecords[pipelineState] = record;
	ReleaseSRWLockExclusive(&gModLock);
}

static bool HasShaderOverrideLocked(uint64_t hash)
{
	return gShaderOverrides.find(hash) != gShaderOverrides.end();
}

static bool GraphicsPsoNeedsReplacementLocked(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc)
{
	if (desc.VS.pShaderBytecode && desc.VS.BytecodeLength &&
	    HasShaderOverrideLocked(DX12ModHashShaderBytecode(desc.VS.pShaderBytecode, desc.VS.BytecodeLength)))
		return true;
	if (desc.PS.pShaderBytecode && desc.PS.BytecodeLength &&
	    HasShaderOverrideLocked(DX12ModHashShaderBytecode(desc.PS.pShaderBytecode, desc.PS.BytecodeLength)))
		return true;
	return false;
}

static bool ComputePsoNeedsReplacementLocked(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc)
{
	return desc.CS.pShaderBytecode && desc.CS.BytecodeLength &&
		HasShaderOverrideLocked(DX12ModHashShaderBytecode(desc.CS.pShaderBytecode, desc.CS.BytecodeLength));
}

static ID3D12PipelineState *CreateGraphicsReplacement(DX12StoredPso *record)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = record->graphicsDesc;
	D3D12_SHADER_BYTECODE vs = {};
	D3D12_SHADER_BYTECODE ps = {};
	std::vector<unsigned char> vsBytes;
	std::vector<unsigned char> psBytes;
	bool changed = false;

	if (DX12ModReplaceShaderBytecode("vs", desc.VS, &vs, &vsBytes)) {
		desc.VS = vs;
		changed = true;
	}
	if (DX12ModReplaceShaderBytecode("ps", desc.PS, &ps, &psBytes)) {
		desc.PS = ps;
		changed = true;
	}
	if (!changed)
		return nullptr;

	ID3D12PipelineState *replacement = nullptr;
	HRESULT hr = DX12CreateGraphicsPipelineStateOriginal(
		record->device, &desc, IID_PPV_ARGS(&replacement));
	DX12LogDebugJsonFunc("DX12ReplacementPsoCreate",
		"\"kind\":\"graphics\",\"hr\":\"0x%lx\",\"pso\":\"%p\"",
		hr, replacement);
	return SUCCEEDED(hr) ? replacement : nullptr;
}

static ID3D12PipelineState *CreateComputeReplacement(DX12StoredPso *record)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc = record->computeDesc;
	D3D12_SHADER_BYTECODE cs = {};
	std::vector<unsigned char> csBytes;
	if (!DX12ModReplaceShaderBytecode("cs", desc.CS, &cs, &csBytes))
		return nullptr;
	desc.CS = cs;

	ID3D12PipelineState *replacement = nullptr;
	HRESULT hr = DX12CreateComputePipelineStateOriginal(
		record->device, &desc, IID_PPV_ARGS(&replacement));
	DX12LogDebugJsonFunc("DX12ReplacementPsoCreate",
		"\"kind\":\"compute\",\"hr\":\"0x%lx\",\"pso\":\"%p\"",
		hr, replacement);
	return SUCCEEDED(hr) ? replacement : nullptr;
}

ID3D12PipelineState *DX12ModGetReplacementPipelineState(ID3D12PipelineState *pipelineState)
{
	if (!pipelineState || !DX12ModHasActiveShaderOverrides())
		return nullptr;

	DX12StoredPso createRecord;
	bool shouldCreate = false;
	UINT64 generation = 0;

	AcquireSRWLockExclusive(&gModLock);
	auto it = gPsoRecords.find(pipelineState);
	if (it == gPsoRecords.end()) {
		ReleaseSRWLockExclusive(&gModLock);
		return nullptr;
	}

	DX12StoredPso &record = it->second;
	generation = gReloadGeneration;
	if (record.replacement && record.replacementGeneration == generation) {
		ID3D12PipelineState *replacement = record.replacement;
		ReleaseSRWLockExclusive(&gModLock);
		return replacement;
	}

	if (record.replacement) {
		record.replacement->Release();
		record.replacement = nullptr;
		record.replacementGeneration = 0;
	}

	bool needsReplacement = record.kind == DX12PsoKind::Graphics ?
		GraphicsPsoNeedsReplacementLocked(record.graphicsDesc) :
		ComputePsoNeedsReplacementLocked(record.computeDesc);
	if (!needsReplacement) {
		ReleaseSRWLockExclusive(&gModLock);
		return nullptr;
	}

	createRecord.kind = record.kind;
	createRecord.device = record.device;
	createRecord.graphicsRootSignature = record.graphicsRootSignature;
	createRecord.computeRootSignature = record.computeRootSignature;
	createRecord.graphicsDesc = record.graphicsDesc;
	createRecord.computeDesc = record.computeDesc;
	createRecord.vsBytecode = record.vsBytecode;
	createRecord.psBytecode = record.psBytecode;
	createRecord.dsBytecode = record.dsBytecode;
	createRecord.hsBytecode = record.hsBytecode;
	createRecord.gsBytecode = record.gsBytecode;
	createRecord.csBytecode = record.csBytecode;
	if (!createRecord.vsBytecode.empty())
		createRecord.graphicsDesc.VS.pShaderBytecode = createRecord.vsBytecode.data();
	if (!createRecord.psBytecode.empty())
		createRecord.graphicsDesc.PS.pShaderBytecode = createRecord.psBytecode.data();
	if (!createRecord.dsBytecode.empty())
		createRecord.graphicsDesc.DS.pShaderBytecode = createRecord.dsBytecode.data();
	if (!createRecord.hsBytecode.empty())
		createRecord.graphicsDesc.HS.pShaderBytecode = createRecord.hsBytecode.data();
	if (!createRecord.gsBytecode.empty())
		createRecord.graphicsDesc.GS.pShaderBytecode = createRecord.gsBytecode.data();
	if (!createRecord.csBytecode.empty())
		createRecord.computeDesc.CS.pShaderBytecode = createRecord.csBytecode.data();
	if (createRecord.device)
		createRecord.device->AddRef();
	if (createRecord.graphicsRootSignature)
		createRecord.graphicsRootSignature->AddRef();
	if (createRecord.computeRootSignature)
		createRecord.computeRootSignature->AddRef();
	shouldCreate = true;
	ReleaseSRWLockExclusive(&gModLock);

	if (!shouldCreate)
		return nullptr;

	ID3D12PipelineState *newReplacement = createRecord.kind == DX12PsoKind::Graphics ?
		CreateGraphicsReplacement(&createRecord) :
		CreateComputeReplacement(&createRecord);
	if (createRecord.device)
		createRecord.device->Release();
	if (createRecord.graphicsRootSignature)
		createRecord.graphicsRootSignature->Release();
	if (createRecord.computeRootSignature)
		createRecord.computeRootSignature->Release();
	if (!newReplacement)
		return nullptr;

	AcquireSRWLockExclusive(&gModLock);
	it = gPsoRecords.find(pipelineState);
	if (it == gPsoRecords.end() || gReloadGeneration != generation) {
		ReleaseSRWLockExclusive(&gModLock);
		newReplacement->Release();
		return nullptr;
	}
	if (it->second.replacement)
		it->second.replacement->Release();
	it->second.replacement = newReplacement;
	it->second.replacementGeneration = generation;
	ReleaseSRWLockExclusive(&gModLock);
	return newReplacement;
}
