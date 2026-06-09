#include "DX12ShaderDump.h"

#include <d3dcompiler.h>
#include <dxcapi.h>
#include <Shlwapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "DX12ResourceTracker.h"
#include "DX12State.h"

struct ShaderRecord
{
	std::string stage;
	uint64_t hash = 0;
	std::vector<uint8_t> bytecode;
	UINT64 firstPsoIndex = 0;
	UINT64 useCount = 0;
};

struct PsoRecord
{
	std::string kind;
	UINT64 index = 0;
	std::vector<std::string> shaders;
};

static SRWLOCK gDumpLock = SRWLOCK_INIT;
static std::unordered_map<std::string, ShaderRecord> gShaders;
static std::vector<PsoRecord> gPsos;
static std::unordered_map<ID3D12PipelineState*, DX12PsoShaderInfo> gPsoShaderInfo;
static UINT64 gPsoSerial = 0;

typedef HRESULT(WINAPI *PFN_D3D_DISASSEMBLE)(
	LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

static HMODULE gD3DCompiler = nullptr;
static PFN_D3D_DISASSEMBLE gD3DDisassemble = nullptr;

static HMODULE gDXCompiler = nullptr;
static DxcCreateInstanceProc gDxcCreateInstance = nullptr;
static IDxcCompiler3 *gDxcCompiler = nullptr;

static constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
	return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
		(static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
		(static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
		(static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

static bool ReadU32LE(const uint8_t *data, size_t size, size_t offset, uint32_t *value)
{
	if (!value || offset + sizeof(uint32_t) > size)
		return false;
	memcpy(value, data + offset, sizeof(uint32_t));
	return true;
}

static bool ShaderHasChunk(const std::vector<uint8_t> &bytecode, uint32_t fourCC)
{
	if (bytecode.size() < 32 || memcmp(bytecode.data(), "DXBC", 4))
		return false;

	uint32_t chunkCount = 0;
	if (!ReadU32LE(bytecode.data(), bytecode.size(), 28, &chunkCount))
		return false;

	for (uint32_t i = 0; i < chunkCount; ++i) {
		uint32_t chunkOffset = 0;
		if (!ReadU32LE(bytecode.data(), bytecode.size(), 32 + sizeof(uint32_t) * i, &chunkOffset))
			return false;
		if (chunkOffset + sizeof(uint32_t) > bytecode.size())
			continue;
		uint32_t chunkFourCC = 0;
		memcpy(&chunkFourCC, bytecode.data() + chunkOffset, sizeof(chunkFourCC));
		if (chunkFourCC == fourCC)
			return true;
	}

	return false;
}

static bool ShaderIsDXIL(const ShaderRecord &record)
{
	return ShaderHasChunk(record.bytecode, MakeFourCC('D', 'X', 'I', 'L'));
}

static uint64_t Fnv1a64(const void *data, size_t size)
{
	const uint8_t *bytes = static_cast<const uint8_t*>(data);
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

static std::string MakeShaderKey(const char *stage, uint64_t hash)
{
	char key[64];
	sprintf_s(key, "%s_%016llx", stage, static_cast<unsigned long long>(hash));
	return key;
}

static bool GetDumpDirectory(wchar_t *path, size_t pathCount)
{
	if (!GetModuleFileNameW(DX12GetModule(), path, static_cast<DWORD>(pathCount)))
		return false;
	PathRemoveFileSpecW(path);
	PathAppendW(path, L"ShaderDumpDX12");
	return CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void RecordShaderLocked(
	const D3D12_SHADER_BYTECODE &bytecode, const char *stage,
	UINT64 psoIndex, PsoRecord &pso, DX12PsoShaderInfo *info)
{
	if (!bytecode.pShaderBytecode || bytecode.BytecodeLength == 0)
		return;

	uint64_t hash = Fnv1a64(bytecode.pShaderBytecode, bytecode.BytecodeLength);
	std::string key = MakeShaderKey(stage, hash);
	pso.shaders.push_back(key);
	if (info) {
		if (!strcmp(stage, "vs")) {
			info->hasVS = true;
			info->vs = hash;
		} else if (!strcmp(stage, "ps")) {
			info->hasPS = true;
			info->ps = hash;
		} else if (!strcmp(stage, "cs")) {
			info->hasCS = true;
			info->cs = hash;
		}
	}

	auto it = gShaders.find(key);
	if (it == gShaders.end()) {
		ShaderRecord record;
		record.stage = stage;
		record.hash = hash;
		record.bytecode.resize(bytecode.BytecodeLength);
		memcpy(record.bytecode.data(), bytecode.pShaderBytecode, bytecode.BytecodeLength);
		record.firstPsoIndex = psoIndex;
		record.useCount = 1;
		gShaders.emplace(key, std::move(record));
	} else {
		it->second.useCount++;
	}
}

static bool WriteShaderFile(const wchar_t *dir, const ShaderRecord &record)
{
	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\%016llx-%S.bin",
		dir, static_cast<unsigned long long>(record.hash), record.stage.c_str());

	FILE *file = _wfsopen(path, L"wb", _SH_DENYNO);
	if (!file)
		return false;
	fwrite(record.bytecode.data(), 1, record.bytecode.size(), file);
	fclose(file);
	return true;
}

static bool EnsureD3DDisassemble()
{
	if (gD3DDisassemble)
		return true;

	if (!gD3DCompiler) {
		wchar_t path[MAX_PATH];
		if (GetSystemDirectoryW(path, MAX_PATH)) {
			PathAppendW(path, L"d3dcompiler_47.dll");
			gD3DCompiler = LoadLibraryW(path);
		}
		if (!gD3DCompiler)
			gD3DCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
	}

	if (!gD3DCompiler) {
		DX12Log("Failed to load d3dcompiler_47.dll for shader disassembly, error=%lu\n",
			GetLastError());
		return false;
	}

	gD3DDisassemble = reinterpret_cast<PFN_D3D_DISASSEMBLE>(
		GetProcAddress(gD3DCompiler, "D3DDisassemble"));
	if (!gD3DDisassemble) {
		DX12Log("Failed to find D3DDisassemble in d3dcompiler_47.dll\n");
		return false;
	}

	return true;
}

static bool EnsureDXILDisassemble()
{
	if (gDxcCompiler)
		return true;

	if (!gDXCompiler) {
		const wchar_t *sdkPath =
			L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\dxcompiler.dll";
		if (PathFileExistsW(sdkPath))
			gDXCompiler = LoadLibraryW(sdkPath);
		if (!gDXCompiler)
			gDXCompiler = LoadLibraryW(L"dxcompiler.dll");
	}

	if (!gDXCompiler) {
		DX12Log("Failed to load dxcompiler.dll for DXIL disassembly, error=%lu\n",
			GetLastError());
		return false;
	}

	gDxcCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(
		GetProcAddress(gDXCompiler, "DxcCreateInstance"));
	if (!gDxcCreateInstance) {
		DX12Log("Failed to find DxcCreateInstance in dxcompiler.dll\n");
		return false;
	}

	HRESULT hr = gDxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&gDxcCompiler));
	if (FAILED(hr) || !gDxcCompiler) {
		DX12Log("Failed to create IDxcCompiler3 for DXIL disassembly hr=0x%lx\n", hr);
		return false;
	}

	return true;
}

static bool WriteDXBCDisassemblyFile(const wchar_t *path, const ShaderRecord &record)
{
	if (!EnsureD3DDisassemble())
		return false;

	ID3DBlob *disassembly = nullptr;
	const UINT flags = D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS | D3D_DISASM_DISABLE_DEBUG_INFO;
	HRESULT hr = gD3DDisassemble(
		record.bytecode.data(), record.bytecode.size(), flags, nullptr, &disassembly);
	if (FAILED(hr) || !disassembly) {
		DX12Log("D3DDisassemble failed for %016llx-%s.bin hr=0x%lx\n",
			static_cast<unsigned long long>(record.hash), record.stage.c_str(), hr);
		return false;
	}

	FILE *file = _wfsopen(path, L"wb", _SH_DENYNO);
	if (!file) {
		disassembly->Release();
		return false;
	}

	fwrite(disassembly->GetBufferPointer(), 1, disassembly->GetBufferSize(), file);
	fclose(file);
	disassembly->Release();
	return true;
}

static bool WriteDXILDisassemblyFile(const wchar_t *path, const ShaderRecord &record)
{
	if (!EnsureDXILDisassemble())
		return false;

	DxcBuffer buffer = {};
	buffer.Ptr = record.bytecode.data();
	buffer.Size = record.bytecode.size();
	buffer.Encoding = DXC_CP_ACP;

	IDxcResult *result = nullptr;
	HRESULT hr = gDxcCompiler->Disassemble(&buffer, IID_PPV_ARGS(&result));
	if (FAILED(hr) || !result) {
		DX12Log("DXIL Disassemble failed for %016llx-%s.bin hr=0x%lx\n",
			static_cast<unsigned long long>(record.hash), record.stage.c_str(), hr);
		return false;
	}

	HRESULT status = S_OK;
	result->GetStatus(&status);
	if (FAILED(status)) {
		DX12Log("DXIL Disassemble status failed for %016llx-%s.bin status=0x%lx\n",
			static_cast<unsigned long long>(record.hash), record.stage.c_str(), status);
		result->Release();
		return false;
	}

	IDxcBlobUtf8 *disassembly = nullptr;
	hr = result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&disassembly), nullptr);
	if (FAILED(hr) || !disassembly) {
		DX12Log("DXIL Disassemble output missing for %016llx-%s.bin hr=0x%lx\n",
			static_cast<unsigned long long>(record.hash), record.stage.c_str(), hr);
		result->Release();
		return false;
	}

	FILE *file = _wfsopen(path, L"wb", _SH_DENYNO);
	if (!file) {
		disassembly->Release();
		result->Release();
		return false;
	}

	fwrite(disassembly->GetStringPointer(), 1, disassembly->GetStringLength(), file);
	fclose(file);
	disassembly->Release();
	result->Release();
	return true;
}

static bool WriteShaderDisassemblyFile(const wchar_t *dir, const ShaderRecord &record)
{
	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\%016llx-%S.asm.txt",
		dir, static_cast<unsigned long long>(record.hash), record.stage.c_str());

	if (ShaderIsDXIL(record))
		return WriteDXILDisassemblyFile(path, record);
	return WriteDXBCDisassemblyFile(path, record);
}

struct ShaderAnalysisRecord
{
	ShaderRecord shader;
	bool dxil = false;
	size_t asmSize = 0;
	UINT cbuffers = 0;
	UINT textures = 0;
	UINT samplers = 0;
	UINT uavs = 0;
	UINT samples = 0;
	UINT textureLoads = 0;
	UINT rawBufferLoads = 0;
	UINT cbufferLoads = 0;
	UINT storeOutputs = 0;
	bool hasDiscard = false;
	bool writesDepth = false;
	std::string tags;
};

struct StageAnalysisStats
{
	UINT shaders = 0;
	UINT dxil = 0;
	UINT dxbc = 0;
	UINT sampled = 0;
	UINT uav = 0;
	UINT discard = 0;
	UINT depth = 0;
	size_t bytecodeBytes = 0;
	size_t asmBytes = 0;
};

static bool ReadTextFile(const wchar_t *path, std::string *text)
{
	if (!text)
		return false;

	FILE *file = _wfsopen(path, L"rb", _SH_DENYNO);
	if (!file)
		return false;

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	if (size < 0) {
		fclose(file);
		return false;
	}
	fseek(file, 0, SEEK_SET);

	text->resize(static_cast<size_t>(size));
	if (size > 0)
		fread(&(*text)[0], 1, static_cast<size_t>(size), file);
	fclose(file);
	return true;
}

static std::string LowerAscii(const std::string &text)
{
	std::string lower = text;
	for (char &ch : lower) {
		if (ch >= 'A' && ch <= 'Z')
			ch = static_cast<char>(ch - 'A' + 'a');
	}
	return lower;
}

static UINT CountSubstring(const std::string &text, const char *needle)
{
	if (!needle || !needle[0])
		return 0;

	UINT count = 0;
	size_t pos = 0;
	const size_t needleLen = strlen(needle);
	while ((pos = text.find(needle, pos)) != std::string::npos) {
		count++;
		pos += needleLen;
	}
	return count;
}

static UINT CountResourceBindingRows(const std::string &lowerText, const char *kind)
{
	UINT count = 0;
	bool inBindings = false;
	size_t pos = 0;
	while (pos < lowerText.size()) {
		size_t end = lowerText.find('\n', pos);
		if (end == std::string::npos)
			end = lowerText.size();
		std::string line = lowerText.substr(pos, end - pos);

		if (line.find("resource bindings:") != std::string::npos)
			inBindings = true;
		else if (inBindings && line.find(kind) != std::string::npos)
			count++;

		pos = end + 1;
	}
	return count;
}

static void AppendTag(std::string *tags, const char *tag)
{
	if (!tags || !tag || !tag[0])
		return;
	if (!tags->empty())
		*tags += "|";
	*tags += tag;
}

static void BuildShaderAsmPath(const wchar_t *dir, const ShaderRecord &shader, wchar_t *path, size_t pathCount)
{
	swprintf_s(path, pathCount, L"%s\\%016llx-%S.asm.txt",
		dir, static_cast<unsigned long long>(shader.hash), shader.stage.c_str());
}

static void BuildShaderAnalysisRecord(
	const wchar_t *dir, const ShaderRecord &shader, ShaderAnalysisRecord *analysis)
{
	if (!analysis)
		return;

	analysis->shader = shader;
	analysis->dxil = ShaderIsDXIL(shader);

	wchar_t asmPath[MAX_PATH];
	BuildShaderAsmPath(dir, shader, asmPath, ARRAYSIZE(asmPath));

	std::string text;
	if (!ReadTextFile(asmPath, &text))
		return;
	analysis->asmSize = text.size();

	std::string lower = LowerAscii(text);
	analysis->cbuffers = CountResourceBindingRows(lower, " cbuffer ");
	analysis->textures = CountResourceBindingRows(lower, " texture ");
	analysis->samplers = CountResourceBindingRows(lower, " sampler ");
	analysis->uavs =
		CountResourceBindingRows(lower, " uav ") +
		CountResourceBindingRows(lower, " rw") +
		CountSubstring(lower, "dx.op.bufferstore") +
		CountSubstring(lower, "dx.op.texturestore");
	analysis->samples =
		CountSubstring(lower, "dx.op.sample") +
		CountSubstring(lower, "\nsample") +
		CountSubstring(lower, "\nsample_l") +
		CountSubstring(lower, "\nsample_d");
	analysis->textureLoads =
		CountSubstring(lower, "dx.op.textureload") +
		CountSubstring(lower, "\nld ");
	analysis->rawBufferLoads = CountSubstring(lower, "dx.op.rawbufferload");
	analysis->cbufferLoads =
		CountSubstring(lower, "dx.op.cbufferload") +
		CountSubstring(lower, "cbufferloadlegacy");
	analysis->storeOutputs =
		CountSubstring(lower, "dx.op.storeoutput") +
		CountSubstring(lower, "\nmov o");
	analysis->hasDiscard =
		lower.find("dx.op.discard") != std::string::npos ||
		lower.find("\ndiscard") != std::string::npos;
	analysis->writesDepth =
		lower.find("sv_depth") != std::string::npos ||
		lower.find("depthoutput=1") != std::string::npos;

	AppendTag(&analysis->tags, analysis->dxil ? "dxil" : "dxbc");
	AppendTag(&analysis->tags, shader.stage.c_str());
	if (analysis->samples)
		AppendTag(&analysis->tags, "samples-texture");
	if (analysis->textureLoads || analysis->rawBufferLoads)
		AppendTag(&analysis->tags, "loads-resource");
	if (analysis->cbufferLoads || analysis->cbuffers)
		AppendTag(&analysis->tags, "uses-cbuffer");
	if (analysis->uavs)
		AppendTag(&analysis->tags, "writes-uav");
	if (analysis->storeOutputs)
		AppendTag(&analysis->tags, "writes-output");
	if (analysis->hasDiscard)
		AppendTag(&analysis->tags, "discard");
	if (analysis->writesDepth)
		AppendTag(&analysis->tags, "depth");
	if (lower.find("canvas") != std::string::npos || lower.find("atlas_texture") != std::string::npos)
		AppendTag(&analysis->tags, "ui-canvas-candidate");
	if (analysis->asmSize > 100000)
		AppendTag(&analysis->tags, "large");
}

static int StageIndex(const std::string &stage)
{
	if (stage == "vs")
		return 0;
	if (stage == "ps")
		return 1;
	if (stage == "cs")
		return 2;
	return 3;
}

static void WriteShaderAnalysisFile(
	const wchar_t *dir, const std::vector<ShaderRecord> &shaders,
	const std::vector<PsoRecord> &psos)
{
	std::vector<ShaderAnalysisRecord> analyses;
	analyses.reserve(shaders.size());
	for (const ShaderRecord &shader : shaders) {
		ShaderAnalysisRecord analysis;
		BuildShaderAnalysisRecord(dir, shader, &analysis);
		analyses.push_back(std::move(analysis));
	}

	std::sort(analyses.begin(), analyses.end(),
		[](const ShaderAnalysisRecord &a, const ShaderAnalysisRecord &b) {
			int stageA = StageIndex(a.shader.stage);
			int stageB = StageIndex(b.shader.stage);
			if (stageA != stageB)
				return stageA < stageB;
			return a.shader.hash < b.shader.hash;
		});

	StageAnalysisStats stageStats[4] = {};
	UINT dxilCount = 0;
	UINT dxbcCount = 0;
	UINT sampledCount = 0;
	UINT uavCount = 0;
	UINT discardCount = 0;
	UINT depthCount = 0;
	for (const ShaderAnalysisRecord &analysis : analyses) {
		int idx = StageIndex(analysis.shader.stage);
		if (idx < 0 || idx > 3)
			idx = 3;
		StageAnalysisStats &stats = stageStats[idx];
		stats.shaders++;
		stats.bytecodeBytes += analysis.shader.bytecode.size();
		stats.asmBytes += analysis.asmSize;
		if (analysis.dxil) {
			stats.dxil++;
			dxilCount++;
		} else {
			stats.dxbc++;
			dxbcCount++;
		}
		if (analysis.samples) {
			stats.sampled++;
			sampledCount++;
		}
		if (analysis.uavs) {
			stats.uav++;
			uavCount++;
		}
		if (analysis.hasDiscard) {
			stats.discard++;
			discardCount++;
		}
		if (analysis.writesDepth) {
			stats.depth++;
			depthCount++;
		}
	}

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\ShaderAnalysis.txt", dir);
	FILE *file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;

	fprintf(file, "DX12 Shader Analysis\n");
	fprintf(file, "====================\n");
	fprintf(file, "shaders=%zu psos=%zu dxil=%u dxbc=%u sampled=%u uav=%u discard=%u depth=%u\n\n",
		analyses.size(), psos.size(), dxilCount, dxbcCount,
		sampledCount, uavCount, discardCount, depthCount);

	const char *stageNames[] = { "vs", "ps", "cs", "other" };
	fprintf(file, "Stage Summary\n");
	fprintf(file, "stage,shaders,dxil,dxbc,bytecode_bytes,asm_bytes,sampled,uav,discard,depth\n");
	for (int i = 0; i < 4; ++i) {
		const StageAnalysisStats &stats = stageStats[i];
		if (!stats.shaders)
			continue;
		fprintf(file, "%s,%u,%u,%u,%zu,%zu,%u,%u,%u,%u\n",
			stageNames[i], stats.shaders, stats.dxil, stats.dxbc,
			stats.bytecodeBytes, stats.asmBytes, stats.sampled,
			stats.uav, stats.discard, stats.depth);
	}

	std::vector<ShaderAnalysisRecord> bySize = analyses;
	std::sort(bySize.begin(), bySize.end(),
		[](const ShaderAnalysisRecord &a, const ShaderAnalysisRecord &b) {
			return a.asmSize > b.asmSize;
		});

	fprintf(file, "\nLargest ASM Files\n");
	fprintf(file, "hash,stage,asm_size,bytecode_size,tags\n");
	for (size_t i = 0; i < bySize.size() && i < 20; ++i) {
		const ShaderAnalysisRecord &analysis = bySize[i];
		fprintf(file, "%016llx,%s,%zu,%zu,%s\n",
			static_cast<unsigned long long>(analysis.shader.hash),
			analysis.shader.stage.c_str(),
			analysis.asmSize,
			analysis.shader.bytecode.size(),
			analysis.tags.c_str());
	}

	fprintf(file, "\nShader Details\n");
	fprintf(file,
		"hash,stage,container,size,asm_size,uses,first_pso,cbuffers,textures,samplers,uavs,"
		"samples,texture_loads,raw_buffer_loads,cbuffer_loads,store_outputs,discard,depth,tags,file,asm_file\n");
	for (const ShaderAnalysisRecord &analysis : analyses) {
		fprintf(file,
			"%016llx,%s,%s,%zu,%zu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%016llx-%s.bin,%016llx-%s.asm.txt\n",
			static_cast<unsigned long long>(analysis.shader.hash),
			analysis.shader.stage.c_str(),
			analysis.dxil ? "dxil" : "dxbc",
			analysis.shader.bytecode.size(),
			analysis.asmSize,
			static_cast<unsigned long long>(analysis.shader.useCount),
			static_cast<unsigned long long>(analysis.shader.firstPsoIndex),
			analysis.cbuffers,
			analysis.textures,
			analysis.samplers,
			analysis.uavs,
			analysis.samples,
			analysis.textureLoads,
			analysis.rawBufferLoads,
			analysis.cbufferLoads,
			analysis.storeOutputs,
			analysis.hasDiscard ? 1 : 0,
			analysis.writesDepth ? 1 : 0,
			analysis.tags.c_str(),
			static_cast<unsigned long long>(analysis.shader.hash),
			analysis.shader.stage.c_str(),
			static_cast<unsigned long long>(analysis.shader.hash),
			analysis.shader.stage.c_str());
	}
	fclose(file);

	DX12Log("Shader analysis written: %S entries=%zu\n", path, analyses.size());
}

static size_t AlignUp(size_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static size_t PipelineStateStreamPayloadSize(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
	switch (type) {
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
		return sizeof(ID3D12RootSignature*);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
		return sizeof(D3D12_SHADER_BYTECODE);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
		return sizeof(D3D12_STREAM_OUTPUT_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
		return sizeof(D3D12_BLEND_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
		return sizeof(UINT);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
		return sizeof(D3D12_RASTERIZER_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
		return sizeof(D3D12_DEPTH_STENCIL_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
		return sizeof(D3D12_INPUT_LAYOUT_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
		return sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
		return sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
		return sizeof(D3D12_RT_FORMAT_ARRAY);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
		return sizeof(DXGI_FORMAT);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
		return sizeof(DXGI_SAMPLE_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
		return sizeof(UINT);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
		return sizeof(D3D12_CACHED_PIPELINE_STATE);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
		return sizeof(D3D12_PIPELINE_STATE_FLAGS);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
		return sizeof(D3D12_DEPTH_STENCIL_DESC1);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
		return sizeof(D3D12_VIEW_INSTANCING_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
		return sizeof(D3D12_DEPTH_STENCIL_DESC2);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
		return sizeof(D3D12_RASTERIZER_DESC1);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
		return sizeof(D3D12_RASTERIZER_DESC2);
	default:
		return 0;
	}
}

static size_t PipelineStateStreamPayloadAlignment(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
	switch (type) {
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
		return alignof(ID3D12RootSignature*);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
		return alignof(D3D12_SHADER_BYTECODE);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
		return alignof(D3D12_STREAM_OUTPUT_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
		return alignof(D3D12_BLEND_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
		return alignof(UINT);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
		return alignof(D3D12_RASTERIZER_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
		return alignof(D3D12_DEPTH_STENCIL_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
		return alignof(D3D12_INPUT_LAYOUT_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
		return alignof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
		return alignof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
		return alignof(D3D12_RT_FORMAT_ARRAY);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
		return alignof(DXGI_FORMAT);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
		return alignof(DXGI_SAMPLE_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
		return alignof(UINT);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
		return alignof(D3D12_CACHED_PIPELINE_STATE);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
		return alignof(D3D12_PIPELINE_STATE_FLAGS);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
		return alignof(D3D12_DEPTH_STENCIL_DESC1);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
		return alignof(D3D12_VIEW_INSTANCING_DESC);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
		return alignof(D3D12_DEPTH_STENCIL_DESC2);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
		return alignof(D3D12_RASTERIZER_DESC1);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
		return alignof(D3D12_RASTERIZER_DESC2);
	default:
		return 1;
	}
}

static const char *PipelineStateStreamShaderStage(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
	switch (type) {
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
		return "vs";
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
		return "ps";
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
		return "ds";
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
		return "hs";
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
		return "gs";
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
		return "cs";
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
		return "as";
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
		return "ms";
	default:
		return nullptr;
	}
}

void DX12RecordGraphicsPipelineState(
	ID3D12PipelineState *pipelineState, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
	if (!desc)
		return;

	AcquireSRWLockExclusive(&gDumpLock);
	UINT64 psoIndex = ++gPsoSerial;
	PsoRecord pso;
	DX12PsoShaderInfo info = {};
	info.psoIndex = psoIndex;
	pso.kind = "graphics";
	pso.index = psoIndex;
	DX12RecordPsoRootSignature(psoIndex, pso.kind.c_str(), pipelineState, desc->pRootSignature);
	RecordShaderLocked(desc->VS, "vs", psoIndex, pso, &info);
	RecordShaderLocked(desc->PS, "ps", psoIndex, pso, &info);
	RecordShaderLocked(desc->DS, "ds", psoIndex, pso, nullptr);
	RecordShaderLocked(desc->HS, "hs", psoIndex, pso, nullptr);
	RecordShaderLocked(desc->GS, "gs", psoIndex, pso, nullptr);
	const size_t shadersInPso = pso.shaders.size();
	gPsos.push_back(std::move(pso));
	if (pipelineState)
		gPsoShaderInfo[pipelineState] = info;
	UINT64 shaderCount = gShaders.size();
	UINT64 psoCount = gPsos.size();
	ReleaseSRWLockExclusive(&gDumpLock);

	DX12Log("Recorded graphics PSO #%llu shaders=%llu cachedShaders=%llu cachedPSOs=%llu\n",
		static_cast<unsigned long long>(psoIndex),
		static_cast<unsigned long long>(shadersInPso),
		static_cast<unsigned long long>(shaderCount),
		static_cast<unsigned long long>(psoCount));
	DX12SetOverlayStatus(L"3DMigoto DX12 hook alive | cached shaders ready");
}

void DX12RecordComputePipelineState(
	ID3D12PipelineState *pipelineState, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc)
{
	if (!desc)
		return;

	AcquireSRWLockExclusive(&gDumpLock);
	UINT64 psoIndex = ++gPsoSerial;
	PsoRecord pso;
	DX12PsoShaderInfo info = {};
	info.psoIndex = psoIndex;
	pso.kind = "compute";
	pso.index = psoIndex;
	DX12RecordPsoRootSignature(psoIndex, pso.kind.c_str(), pipelineState, desc->pRootSignature);
	RecordShaderLocked(desc->CS, "cs", psoIndex, pso, &info);
	gPsos.push_back(std::move(pso));
	if (pipelineState)
		gPsoShaderInfo[pipelineState] = info;
	UINT64 shaderCount = gShaders.size();
	UINT64 psoCount = gPsos.size();
	ReleaseSRWLockExclusive(&gDumpLock);

	DX12Log("Recorded compute PSO #%llu cachedShaders=%llu cachedPSOs=%llu\n",
		static_cast<unsigned long long>(psoIndex),
		static_cast<unsigned long long>(shaderCount),
		static_cast<unsigned long long>(psoCount));
	DX12SetOverlayStatus(L"3DMigoto DX12 hook alive | cached shaders ready");
}

void DX12RecordPipelineStateStream(
	ID3D12PipelineState *pipelineState, const D3D12_PIPELINE_STATE_STREAM_DESC *desc)
{
	if (!desc || !desc->pPipelineStateSubobjectStream || desc->SizeInBytes == 0)
		return;

	const uint8_t *stream = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
	size_t offset = 0;
	size_t shadersInPso = 0;

	AcquireSRWLockExclusive(&gDumpLock);
	UINT64 psoIndex = ++gPsoSerial;
	PsoRecord pso;
	DX12PsoShaderInfo info = {};
	info.psoIndex = psoIndex;
	pso.kind = "stream";
	pso.index = psoIndex;
	ID3D12RootSignature *rootSignature = nullptr;

	while (offset + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= desc->SizeInBytes) {
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
			*reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(stream + offset);
		size_t payloadSize = PipelineStateStreamPayloadSize(type);
		size_t payloadAlignment = PipelineStateStreamPayloadAlignment(type);
		size_t payloadOffset = AlignUp(offset + sizeof(type), payloadAlignment);
		if (payloadSize == 0 || payloadOffset + payloadSize > desc->SizeInBytes) {
			DX12Log("Stopped parsing pipeline state stream pso=%llu type=%d offset=%zu size=%zu\n",
				static_cast<unsigned long long>(psoIndex), static_cast<int>(type),
				offset, desc->SizeInBytes);
			break;
		}

		const char *stage = PipelineStateStreamShaderStage(type);
		if (stage) {
			const D3D12_SHADER_BYTECODE *bytecode =
				reinterpret_cast<const D3D12_SHADER_BYTECODE*>(stream + payloadOffset);
			RecordShaderLocked(*bytecode, stage, psoIndex, pso, &info);
		} else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
			rootSignature = *reinterpret_cast<ID3D12RootSignature *const*>(stream + payloadOffset);
		}

		offset = AlignUp(payloadOffset + payloadSize, alignof(void*));
	}

	shadersInPso = pso.shaders.size();
	DX12RecordPsoRootSignature(psoIndex, pso.kind.c_str(), pipelineState, rootSignature);
	gPsos.push_back(std::move(pso));
	if (pipelineState)
		gPsoShaderInfo[pipelineState] = info;
	UINT64 shaderCount = gShaders.size();
	UINT64 psoCount = gPsos.size();
	ReleaseSRWLockExclusive(&gDumpLock);

	DX12Log("Recorded stream PSO #%llu shaders=%llu cachedShaders=%llu cachedPSOs=%llu\n",
		static_cast<unsigned long long>(psoIndex),
		static_cast<unsigned long long>(shadersInPso),
		static_cast<unsigned long long>(shaderCount),
		static_cast<unsigned long long>(psoCount));
	DX12SetOverlayStatus(L"3DMigoto DX12 hook alive | cached shaders ready");
}

bool DX12GetPipelineStateShaderInfo(ID3D12PipelineState *pipelineState, DX12PsoShaderInfo *info)
{
	if (!pipelineState || !info)
		return false;

	AcquireSRWLockShared(&gDumpLock);
	auto it = gPsoShaderInfo.find(pipelineState);
	if (it == gPsoShaderInfo.end()) {
		ReleaseSRWLockShared(&gDumpLock);
		return false;
	}
	*info = it->second;
	ReleaseSRWLockShared(&gDumpLock);
	return true;
}

void DX12DumpCachedShaders()
{
	wchar_t dir[MAX_PATH];
	if (!GetDumpDirectory(dir, ARRAYSIZE(dir))) {
		DX12Log("Failed to create ShaderDumpDX12 directory\n");
		DX12SetOverlayStatus(L"F8 dump failed: cannot create directory");
		return;
	}

	DX12SetOverlayStatus(L"F8 dump requested");

	AcquireSRWLockShared(&gDumpLock);
	std::vector<ShaderRecord> shaders;
	std::vector<PsoRecord> psos = gPsos;
	shaders.reserve(gShaders.size());
	for (const auto &item : gShaders)
		shaders.push_back(item.second);
	ReleaseSRWLockShared(&gDumpLock);

	UINT writtenShaders = 0;
	UINT writtenDisassembly = 0;
	for (const ShaderRecord &shader : shaders) {
		if (WriteShaderFile(dir, shader))
			writtenShaders++;
		if (WriteShaderDisassemblyFile(dir, shader))
			writtenDisassembly++;
	}

	wchar_t usagePath[MAX_PATH];
	swprintf_s(usagePath, L"%s\\ShaderUsage.txt", dir);
	FILE *usage = _wfsopen(usagePath, L"w", _SH_DENYNO);
	if (usage) {
		fprintf(usage, "hash,stage,size,uses,first_pso,file,asm_file\n");
		for (const ShaderRecord &shader : shaders) {
			fprintf(usage, "%016llx,%s,%zu,%llu,%llu,%016llx-%s.bin,%016llx-%s.asm.txt\n",
				static_cast<unsigned long long>(shader.hash),
				shader.stage.c_str(),
				shader.bytecode.size(),
				static_cast<unsigned long long>(shader.useCount),
				static_cast<unsigned long long>(shader.firstPsoIndex),
				static_cast<unsigned long long>(shader.hash),
				shader.stage.c_str(),
				static_cast<unsigned long long>(shader.hash),
				shader.stage.c_str());
		}
		fclose(usage);
	}

	wchar_t psoPath[MAX_PATH];
	swprintf_s(psoPath, L"%s\\pso_log.txt", dir);
	FILE *psoLog = _wfsopen(psoPath, L"w", _SH_DENYNO);
	if (psoLog) {
		for (const PsoRecord &pso : psos) {
			fprintf(psoLog, "pso=%llu kind=%s shaders=",
				static_cast<unsigned long long>(pso.index), pso.kind.c_str());
			for (size_t i = 0; i < pso.shaders.size(); ++i) {
				fprintf(psoLog, "%s%s", i ? ";" : "", pso.shaders[i].c_str());
			}
			fprintf(psoLog, "\n");
		}
		fclose(psoLog);
	}

	WriteShaderAnalysisFile(dir, shaders, psos);
	DX12DumpResourceMetadata(dir);

	wchar_t status[128];
	swprintf_s(status, L"F8 dumped %u shaders / %u asm / %zu PSOs | analysis ready",
		writtenShaders, writtenDisassembly, psos.size());
	DX12SetOverlayStatus(status);
	DX12Log("F8 shader dump complete: dir=%S shaders=%u/%zu asm=%u/%zu psos=%zu analysis=1\n",
		dir, writtenShaders, shaders.size(), writtenDisassembly, shaders.size(), psos.size());
}
