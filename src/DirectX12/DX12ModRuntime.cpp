#include "DX12ModRuntime.h"

#include <Shlwapi.h>
#include <stdio.h>

#include <string>
#include <unordered_map>

#include "DX12State.h"
#include "DX12DeviceHooks.h"
#include "DX12ShaderDump.h"
#include "IniDocument.h"
#include "MigotoIniLoader.h"
#include "MigotoShaderOverride.h"
#include "MigotoTextureOverride.h"

static SRWLOCK gModLock = SRWLOCK_INIT;
static bool gLoaded = false;
static std::wstring gConfigPath;
static std::wstring gBaseDir;
static std::wstring gShaderFixesDir;
static Bunny::ShaderOverrideMap gShaderOverrides;
static Bunny::TextureOverrideMap gTextureOverrides;
static UINT64 gReloadGeneration = 1;

enum class DX12PsoKind
{
	Graphics,
	Compute
};

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
};

static std::unordered_map<ID3D12PipelineState*, DX12StoredPso> gPsoRecords;

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

void DX12ModRuntimeLoad(const wchar_t *configPath)
{
	Bunny::MigotoIniLoadResult iniLoad;
	Bunny::ShaderOverrideMap shaderOverrides;
	Bunny::TextureOverrideMap textureOverrides;

	SetBasePaths(configPath);
	if (!Bunny::LoadMigotoIniWithIncludes(configPath, &iniLoad)) {
		DX12LogJsonFunc("DX12ModRuntime",
			"\"status\":\"config_load_failed\",\"path\":\"%S\",\"error\":\"%S\"",
			configPath ? configPath : L"", iniLoad.document.Error().c_str());
		return;
	}

	Bunny::ParseShaderOverrideSections(iniLoad.document, &shaderOverrides);
	Bunny::ParseTextureOverrideSections(iniLoad.document, &textureOverrides);

	AcquireSRWLockExclusive(&gModLock);
	gShaderOverrides.swap(shaderOverrides);
	gTextureOverrides.swap(textureOverrides);
	gLoaded = true;
	++gReloadGeneration;
	for (auto &item : gPsoRecords) {
		if (item.second.replacement) {
			item.second.replacement->Release();
			item.second.replacement = nullptr;
		}
		item.second.replacementGeneration = 0;
	}
	ReleaseSRWLockExclusive(&gModLock);

	DX12LogJsonFunc("DX12ModRuntime",
		"\"status\":\"loaded\",\"path\":\"%S\",\"iniFiles\":%zu,\"warnings\":%zu,\"shaderOverrides\":%zu,\"textureOverrides\":%zu,\"shaderFixes\":\"%S\",\"generation\":%llu",
		configPath ? configPath : L"", iniLoad.loadedFiles.size(), iniLoad.warnings.size(),
		gShaderOverrides.size(), gTextureOverrides.size(), gShaderFixesDir.c_str(),
		static_cast<unsigned long long>(gReloadGeneration));
	for (const std::wstring &loadedFile : iniLoad.loadedFiles) {
		DX12LogJsonFunc("DX12ModRuntimeIni",
			"\"status\":\"loaded\",\"path\":\"%S\"", loadedFile.c_str());
	}
	for (const std::wstring &warning : iniLoad.warnings) {
		DX12LogJsonFunc("DX12ModRuntimeIni",
			"\"status\":\"warning\",\"message\":\"%S\"", warning.c_str());
	}
}

void DX12ModRuntimeReload()
{
	std::wstring configPath;
	AcquireSRWLockShared(&gModLock);
	configPath = gConfigPath;
	ReleaseSRWLockShared(&gModLock);
	if (configPath.empty()) {
		DX12LogJsonFunc("DX12ModRuntimeReload", "\"status\":\"skipped\",\"reason\":\"empty_config_path\"");
		return;
	}
	DX12LogJsonFunc("DX12ModRuntimeReload", "\"status\":\"begin\",\"path\":\"%S\"", configPath.c_str());
	DX12ModRuntimeLoad(configPath.c_str());
	DX12SetOverlayStatus(L"F10 DX12 mod config reloaded");
}

bool DX12ModReplaceShaderBytecode(
	const char *stage, const D3D12_SHADER_BYTECODE &source,
	D3D12_SHADER_BYTECODE *replacement, std::vector<unsigned char> *storage)
{
	if (!stage || !source.pShaderBytecode || source.BytecodeLength == 0 ||
	    !replacement || !storage)
		return false;

	const uint64_t hash = DX12ModHashShaderBytecode(
		source.pShaderBytecode, source.BytecodeLength);

	std::wstring section;
	std::wstring shaderFixesDir;
	AcquireSRWLockShared(&gModLock);
	auto it = gShaderOverrides.find(hash);
	if (it == gShaderOverrides.end()) {
		ReleaseSRWLockShared(&gModLock);
		return false;
	}
	section = it->second.section;
	shaderFixesDir = gShaderFixesDir;
	ReleaseSRWLockShared(&gModLock);

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\%016llx-%S.bin",
		shaderFixesDir.c_str(), static_cast<unsigned long long>(hash), stage);

	std::vector<unsigned char> bytes;
	if (!ReadFileBytes(path, &bytes)) {
		DX12LogJsonFunc("DX12ShaderOverrideMissingReplacement",
			"\"section\":\"%S\",\"stage\":\"%s\",\"hash\":\"%016llx\",\"path\":\"%S\"",
			section.c_str(), stage, static_cast<unsigned long long>(hash), path);
		return false;
	}

	storage->swap(bytes);
	replacement->pShaderBytecode = storage->data();
	replacement->BytecodeLength = storage->size();
	DX12LogJsonFunc("DX12ShaderOverrideApplied",
		"\"section\":\"%S\",\"stage\":\"%s\",\"hash\":\"%016llx\",\"path\":\"%S\",\"bytes\":%zu",
		section.c_str(), stage, static_cast<unsigned long long>(hash), path, storage->size());
	return true;
}

bool DX12ModHasShaderOverride(uint64_t hash)
{
	AcquireSRWLockShared(&gModLock);
	bool found = gShaderOverrides.find(hash) != gShaderOverrides.end();
	ReleaseSRWLockShared(&gModLock);
	return found;
}

bool DX12ModHasActiveShaderOverrides()
{
	AcquireSRWLockShared(&gModLock);
	bool active = !gShaderOverrides.empty();
	ReleaseSRWLockShared(&gModLock);
	return active;
}

bool DX12ModHasActiveTextureOverrides()
{
	AcquireSRWLockShared(&gModLock);
	bool active = !gTextureOverrides.empty();
	ReleaseSRWLockShared(&gModLock);
	return active;
}

static bool TextureOverrideHasSkipLocked(uint32_t hash, std::wstring *section)
{
	auto it = gTextureOverrides.find(hash);
	if (it == gTextureOverrides.end() || !it->second.handlingSkip)
		return false;
	if (section)
		*section = it->second.section;
	return true;
}

bool DX12ModShouldSkipIa(uint32_t ibHash, const uint32_t *vbHashes, size_t vbHashCount)
{
	if (!DX12ModHasActiveTextureOverrides())
		return false;

	bool skip = false;
	uint32_t matchedHash = 0;
	std::wstring section;
	AcquireSRWLockShared(&gModLock);
	if (ibHash && TextureOverrideHasSkipLocked(ibHash, &section)) {
		skip = true;
		matchedHash = ibHash;
	}
	if (!skip && vbHashes) {
		for (size_t i = 0; i < vbHashCount; ++i) {
			if (!vbHashes[i])
				continue;
			if (TextureOverrideHasSkipLocked(vbHashes[i], &section)) {
				skip = true;
				matchedHash = vbHashes[i];
				break;
			}
		}
	}
	ReleaseSRWLockShared(&gModLock);

	if (skip) {
		DX12LogJsonFunc("DX12TextureOverrideSkip",
			"\"section\":\"%S\",\"hash\":\"%08x\"",
			section.c_str(), matchedHash);
	}
	return skip;
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

bool DX12ModShouldSkipPipelineState(ID3D12PipelineState *pipelineState, bool dispatch)
{
	if (!pipelineState || !DX12ModHasActiveShaderOverrides())
		return false;

	bool skip = false;
	AcquireSRWLockShared(&gModLock);
	auto record = gPsoRecords.find(pipelineState);
	if (record != gPsoRecords.end())
		skip = StoredPsoHasSkipLocked(record->second, dispatch);
	ReleaseSRWLockShared(&gModLock);
	if (skip)
		return true;

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
	DX12LogJsonFunc("DX12ReplacementPsoCreate",
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
	DX12LogJsonFunc("DX12ReplacementPsoCreate",
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
