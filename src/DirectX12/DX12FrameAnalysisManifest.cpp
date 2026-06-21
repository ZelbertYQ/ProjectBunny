#include "DX12FrameAnalysisManifest.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "DX12FrameAnalysis.h"
#include "DX12ShaderDump.h"

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

static const char *TopologyName(D3D12_PRIMITIVE_TOPOLOGY topology)
{
	switch (topology) {
	case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
		return "POINTLIST";
	case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
		return "LINELIST";
	case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
		return "LINESTRIP";
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
		return "TRIANGLELIST";
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
		return "TRIANGLESTRIP";
	default:
		return "UNKNOWN";
	}
}

static const char *DxgiFormatName(UINT format)
{
	switch (format) {
	case DXGI_FORMAT_UNKNOWN:
		return "DXGI_FORMAT_UNKNOWN";
	case DXGI_FORMAT_R16_UINT:
		return "DXGI_FORMAT_R16_UINT";
	case DXGI_FORMAT_R32_UINT:
		return "DXGI_FORMAT_R32_UINT";
	case DXGI_FORMAT_R32_FLOAT:
		return "DXGI_FORMAT_R32_FLOAT";
	case DXGI_FORMAT_R32G32_FLOAT:
		return "DXGI_FORMAT_R32G32_FLOAT";
	case DXGI_FORMAT_R32G32B32_FLOAT:
		return "DXGI_FORMAT_R32G32B32_FLOAT";
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return "DXGI_FORMAT_R32G32B32A32_FLOAT";
	case DXGI_FORMAT_R16G16_FLOAT:
		return "DXGI_FORMAT_R16G16_FLOAT";
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return "DXGI_FORMAT_R16G16B16A16_FLOAT";
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return "DXGI_FORMAT_R8G8B8A8_UNORM";
	case DXGI_FORMAT_BC1_UNORM:
		return "DXGI_FORMAT_BC1_UNORM";
	case DXGI_FORMAT_BC2_UNORM:
		return "DXGI_FORMAT_BC2_UNORM";
	case DXGI_FORMAT_BC3_UNORM:
		return "DXGI_FORMAT_BC3_UNORM";
	case DXGI_FORMAT_BC4_UNORM:
		return "DXGI_FORMAT_BC4_UNORM";
	case DXGI_FORMAT_BC5_UNORM:
		return "DXGI_FORMAT_BC5_UNORM";
	case DXGI_FORMAT_BC6H_UF16:
		return "DXGI_FORMAT_BC6H_UF16";
	case DXGI_FORMAT_BC7_UNORM:
		return "DXGI_FORMAT_BC7_UNORM";
	default:
		return "DXGI_FORMAT_OTHER";
	}
}

static void FormatShaderHash(UINT64 hash, bool hasHash, char *text, size_t textCount)
{
	if (!text || textCount == 0)
		return;
	if (hasHash)
		sprintf_s(text, textCount, "%016llx", static_cast<unsigned long long>(hash));
	else
		sprintf_s(text, textCount, "-");
}

static void FormatTextPath(const wchar_t *filePath, wchar_t *textPath, size_t textPathCount)
{
	if (!textPath || textPathCount == 0)
		return;
	textPath[0] = L'\0';
	if (!filePath || !filePath[0])
		return;
	wcsncpy_s(textPath, textPathCount, filePath, _TRUNCATE);
	wcscat_s(textPath, textPathCount, L".txt");
}

static void FormatFileHash(const wchar_t *filePath, char *hash, size_t hashCount)
{
	if (!hash || hashCount == 0)
		return;
	hash[0] = '\0';
	if (!filePath || !filePath[0])
		return;

	const wchar_t *name = wcsrchr(filePath, L'\\');
	name = name ? name + 1 : filePath;
	size_t i = 0;
	for (; i < hashCount - 1 && i < 8 && name[i] && name[i] != L'-'; ++i)
		hash[i] = static_cast<char>(name[i]);
	hash[i] = '\0';
}

static void JsonEscapeString(char *buf, size_t bufSize, const char *str)
{
	if (!str || !buf || bufSize == 0) {
		if (buf && bufSize > 0)
			*buf = '\0';
		return;
	}
	size_t pos = 0;
	if (pos + 1 < bufSize)
		buf[pos++] = '"';
	for (; *str && pos + 6 < bufSize; ++str) {
		switch (*str) {
		case '"': buf[pos++] = '\\'; buf[pos++] = '"'; break;
		case '\\': buf[pos++] = '\\'; buf[pos++] = '\\'; break;
		case '\b': buf[pos++] = '\\'; buf[pos++] = 'b'; break;
		case '\f': buf[pos++] = '\\'; buf[pos++] = 'f'; break;
		case '\n': buf[pos++] = '\\'; buf[pos++] = 'n'; break;
		case '\r': buf[pos++] = '\\'; buf[pos++] = 'r'; break;
		case '\t': buf[pos++] = '\\'; buf[pos++] = 't'; break;
		default:
			if (static_cast<unsigned char>(*str) < 0x20) {
				sprintf_s(buf + pos, bufSize - pos, "\\u%04x", static_cast<unsigned char>(*str));
				pos += 6;
			} else {
				buf[pos++] = *str;
			}
		}
	}
	if (pos + 1 < bufSize)
		buf[pos++] = '"';
	if (pos < bufSize)
		buf[pos] = '\0';
}

static void JsonEscapeWString(char *buf, size_t bufSize, const wchar_t *str)
{
	if (!buf || bufSize == 0)
		return;
	size_t pos = 0;
	if (pos + 1 < bufSize)
		buf[pos++] = '"';
	if (!str) {
		if (pos + 5 < bufSize) {
			buf[pos++] = 'n';
			buf[pos++] = 'u';
			buf[pos++] = 'l';
			buf[pos++] = 'l';
		}
	} else {
		for (; *str && pos + 6 < bufSize; ++str) {
			wchar_t c = *str;
			if (c < 0x20) {
				sprintf_s(buf + pos, bufSize - pos, "\\u%04x", static_cast<unsigned int>(c));
				pos += 6;
			} else if (c == L'"') {
				buf[pos++] = '\\';
				buf[pos++] = '"';
			} else if (c == L'\\') {
				buf[pos++] = '\\';
				buf[pos++] = '\\';
			} else if (c < 0x80) {
				buf[pos++] = static_cast<char>(c);
			} else if (c < 0x800 && pos + 2 < bufSize) {
				buf[pos++] = static_cast<char>(0xC0 | (c >> 6));
				buf[pos++] = static_cast<char>(0x80 | (c & 0x3F));
			} else if (pos + 3 < bufSize) {
				buf[pos++] = static_cast<char>(0xE0 | (c >> 12));
				buf[pos++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
				buf[pos++] = static_cast<char>(0x80 | (c & 0x3F));
			}
		}
	}
	if (pos + 1 < bufSize && str)
		buf[pos++] = '"';
	if (pos < bufSize)
		buf[pos] = '\0';
}

void DX12FrameAnalysisManifestWriteCall(
	const char *functionName, UINT64 eventSerial, UINT64 drawId, UINT64 dispatchId,
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState,
	const DX12PsoShaderInfo &shaderInfo, D3D12_PRIMITIVE_TOPOLOGY topology,
	UINT vertexCountPerInstance, UINT indexCountPerInstance, UINT startVertexLocation,
	UINT startIndexLocation, INT baseVertexLocation, UINT instanceCount,
	UINT startInstanceLocation, UINT threadGroupCountX, UINT threadGroupCountY,
	UINT threadGroupCountZ, bool indexBufferValid, D3D12_GPU_VIRTUAL_ADDRESS indexBufferGpuVa,
	UINT indexBufferSize, DXGI_FORMAT indexBufferFormat)
{
	char vs[32], ps[32], cs[32];
	FormatShaderHash(shaderInfo.vs, shaderInfo.hasVS, vs, ARRAYSIZE(vs));
	FormatShaderHash(shaderInfo.ps, shaderInfo.hasPS, ps, ARRAYSIZE(ps));
	FormatShaderHash(shaderInfo.cs, shaderInfo.hasCS, cs, ARRAYSIZE(cs));

	const bool isDispatch = functionName && !strcmp(functionName, "dispatch");

	char functionJson[64], vsJson[64], psJson[64], csJson[64], topologyJson[32];
	char cmdlistJson[32], psoJson[32];
	JsonEscapeString(functionJson, sizeof(functionJson), functionName);
	JsonEscapeString(vsJson, sizeof(vsJson), vs);
	JsonEscapeString(psJson, sizeof(psJson), ps);
	JsonEscapeString(csJson, sizeof(csJson), cs);
	JsonEscapeString(topologyJson, sizeof(topologyJson), TopologyName(topology));
	sprintf_s(cmdlistJson, sizeof(cmdlistJson), "\"%p\"", commandList);
	sprintf_s(psoJson, sizeof(psoJson), "\"%p\"", pipelineState);

	char buffer[2048];
	sprintf_s(buffer, sizeof(buffer),
		"{\"type\":\"%s\","
		"\"function\":%s,"
		"\"event\":%llu,"
		"\"draw\":%llu,"
		"\"dispatch\":%llu,"
		"\"cmdlist\":%s,"
		"\"pipeline_state\":%s,"
		"\"pso\":%llu,"
		"\"vs\":%s,"
		"\"ps\":%s,"
		"\"cs\":%s,"
		"\"topology\":%s,"
		"\"vertex_count\":%u,"
		"\"index_count\":%u,"
		"\"start_vertex\":%u,"
		"\"start_index\":%u,"
		"\"base_vertex\":%d,"
		"\"instance_count\":%u,"
		"\"start_instance\":%u,"
		"\"groups_x\":%u,"
		"\"groups_y\":%u,"
		"\"groups_z\":%u,"
		"\"ib_valid\":%s,"
		"\"ib_gpu\":\"0x%llx\","
		"\"ib_bytes\":%u,"
		"\"ib_fmt\":%u}",
		isDispatch ? "call.dispatch" : "call.draw",
		functionJson,
		static_cast<unsigned long long>(eventSerial),
		static_cast<unsigned long long>(drawId),
		static_cast<unsigned long long>(dispatchId),
		cmdlistJson,
		psoJson,
		static_cast<unsigned long long>(shaderInfo.psoIndex),
		vsJson,
		psJson,
		csJson,
		topologyJson,
		vertexCountPerInstance,
		indexCountPerInstance,
		startVertexLocation,
		startIndexLocation,
		baseVertexLocation,
		instanceCount,
		startInstanceLocation,
		threadGroupCountX,
		threadGroupCountY,
		threadGroupCountZ,
		indexBufferValid ? "true" : "false",
		static_cast<unsigned long long>(indexBufferValid ? indexBufferGpuVa : 0),
		indexBufferValid ? indexBufferSize : 0,
		indexBufferValid ? static_cast<UINT>(indexBufferFormat) : 0);

	DX12FrameAnalysisLogJsonLine(buffer);
}

void DX12FrameAnalysisManifestWriteFileDump(
	const wchar_t *filePath, bool isTexture, UINT64 bytes, const char *status,
	const char *note)
{
	char hash[16];
	wchar_t textPath[MAX_PATH];
	FormatFileHash(filePath, hash, ARRAYSIZE(hash));
	FormatTextPath(filePath, textPath, ARRAYSIZE(textPath));

	char statusJson[64], kindJson[16], hashJson[32], noteJson[128];
	char fileJson[512], textJson[512];
	JsonEscapeString(statusJson, sizeof(statusJson), status);
	JsonEscapeString(kindJson, sizeof(kindJson), isTexture ? "texture" : "buffer");
	JsonEscapeString(hashJson, sizeof(hashJson), hash);
	JsonEscapeString(noteJson, sizeof(noteJson), note);
	JsonEscapeWString(fileJson, sizeof(fileJson), filePath);
	if (isTexture) {
		strcpy_s(textJson, sizeof(textJson), "null");
	} else {
		JsonEscapeWString(textJson, sizeof(textJson), textPath);
	}

	char buffer[1024];
	sprintf_s(buffer, sizeof(buffer),
		"{\"type\":\"file.dump\","
		"\"status\":%s,"
		"\"kind\":%s,"
		"\"file\":%s,"
		"\"text\":%s,"
		"\"hash\":%s,"
		"\"bytes\":%llu,"
		"\"note\":%s}",
		statusJson,
		kindJson,
		fileJson,
		textJson,
		hashJson,
		static_cast<unsigned long long>(bytes),
		noteJson);

	DX12FrameAnalysisLogJsonLine(buffer);
}

void DX12FrameAnalysisManifestWriteIaBinding(
	const DX12FrameIaBufferBinding &buffer,
	const D3D12_RESOURCE_DESC &desc, UINT64 sourceOffset, UINT64 copyBytes,
	D3D12_RESOURCE_STATES sourceState, bool hasCurrentState,
	const wchar_t *filePath)
{
	char vs[32], ps[32], cs[32], hash[16];
	char producerCs[32];
	FormatShaderHash(buffer.shaderInfo.vs, buffer.shaderInfo.hasVS, vs, ARRAYSIZE(vs));
	FormatShaderHash(buffer.shaderInfo.ps, buffer.shaderInfo.hasPS, ps, ARRAYSIZE(ps));
	FormatShaderHash(buffer.shaderInfo.cs, buffer.shaderInfo.hasCS, cs, ARRAYSIZE(cs));
	FormatShaderHash(buffer.producerShaderInfo.cs, buffer.producerShaderInfo.hasCS,
		producerCs, ARRAYSIZE(producerCs));
	FormatFileHash(filePath, hash, ARRAYSIZE(hash));

	char vsJson[64], psJson[64], csJson[64], producerCsJson[64];
	char roleJson[32], dimJson[32], fmtNameJson[64], skinSourceJson[64];
	char producerBindJson[64], hashJson[32], fileJson[512], resourceJson[32];
	JsonEscapeString(vsJson, sizeof(vsJson), vs);
	JsonEscapeString(psJson, sizeof(psJson), ps);
	JsonEscapeString(csJson, sizeof(csJson), cs);
	JsonEscapeString(producerCsJson, sizeof(producerCsJson), producerCs);
	JsonEscapeString(roleJson, sizeof(roleJson), buffer.role.c_str());
	JsonEscapeString(dimJson, sizeof(dimJson), ResourceDimensionName(desc.Dimension));
	JsonEscapeString(fmtNameJson, sizeof(fmtNameJson), DxgiFormatName(buffer.format));
	JsonEscapeString(skinSourceJson, sizeof(skinSourceJson),
		buffer.skinningClass.empty() ? "unknown" : buffer.skinningClass.c_str());
	JsonEscapeString(producerBindJson, sizeof(producerBindJson),
		buffer.producerBindSpace.empty() ? "-" : buffer.producerBindSpace.c_str());
	JsonEscapeString(hashJson, sizeof(hashJson), hash);
	JsonEscapeWString(fileJson, sizeof(fileJson), filePath);
	sprintf_s(resourceJson, sizeof(resourceJson), "\"%p\"", buffer.resource.resource);

	char buffer2[2048];
	sprintf_s(buffer2, sizeof(buffer2),
		"{\"type\":\"bind.ia\","
		"\"event\":%llu,"
		"\"draw\":%llu,"
		"\"dispatch\":%llu,"
		"\"pso\":%llu,"
		"\"vs\":%s,"
		"\"ps\":%s,"
		"\"cs\":%s,"
		"\"role\":%s,"
		"\"slot\":%u,"
		"\"resource\":%s,"
		"\"dim\":%s,"
		"\"gpu\":\"0x%llx\","
		"\"offset\":%llu,"
		"\"bytes\":%llu,"
		"\"stride\":%u,"
		"fmt\":%u,"
		"fmt_name\":%s,"
		"\"state\":\"0x%x\","
		"\"state_known\":%s,"
		"\"skin_source\":%s,"
		"\"producer_event\":%llu,"
		"\"producer_draw\":%llu,"
		"\"producer_dispatch\":%llu,"
		"\"producer_pso\":%llu,"
		"\"producer_cs\":%s,"
		"\"producer_bind\":%s,"
		"\"producer_root\":%u,"
		"\"producer_reg\":%u,"
		"\"file\":%s,"
		"\"hash\":%s}",
		static_cast<unsigned long long>(buffer.eventSerial),
		static_cast<unsigned long long>(buffer.drawId),
		static_cast<unsigned long long>(buffer.dispatchId),
		static_cast<unsigned long long>(buffer.psoIndex),
		vsJson,
		psJson,
		csJson,
		roleJson,
		buffer.slot,
		resourceJson,
		dimJson,
		static_cast<unsigned long long>(buffer.gpuVa),
		static_cast<unsigned long long>(sourceOffset),
		static_cast<unsigned long long>(copyBytes),
		buffer.stride,
		buffer.format,
		fmtNameJson,
		static_cast<UINT>(sourceState),
		hasCurrentState ? "true" : "false",
		skinSourceJson,
		static_cast<unsigned long long>(buffer.producerEventSerial),
		static_cast<unsigned long long>(buffer.producerDrawId),
		static_cast<unsigned long long>(buffer.producerDispatchId),
		static_cast<unsigned long long>(buffer.producerPsoIndex),
		producerCsJson,
		producerBindJson,
		buffer.producerRootParameterIndex,
		buffer.producerShaderRegister,
		fileJson,
		hashJson);

	DX12FrameAnalysisLogJsonLine(buffer2);
}

void DX12FrameAnalysisManifestWriteResourceBinding(
	const DX12FrameResourceBinding &binding,
	const D3D12_RESOURCE_DESC &desc, UINT64 sourceOffset, UINT64 copyBytes,
	D3D12_RESOURCE_STATES sourceState, bool hasCurrentState,
	const wchar_t *filePath)
{
	DX12PsoShaderSummary shaders;
	const bool hasShaders = DX12GetPsoShaderSummary(binding.psoIndex, &shaders);
	char vs[32], ps[32], cs[32], hash[16];
	const bool hasVS = binding.shaderInfo.hasVS || (hasShaders && shaders.hasVS);
	const bool hasPS = binding.shaderInfo.hasPS || (hasShaders && shaders.hasPS);
	const bool hasCS = binding.shaderInfo.hasCS || (hasShaders && shaders.hasCS);
	const UINT64 vsHash = binding.shaderInfo.hasVS ? binding.shaderInfo.vs : shaders.vs;
	const UINT64 psHash = binding.shaderInfo.hasPS ? binding.shaderInfo.ps : shaders.ps;
	const UINT64 csHash = binding.shaderInfo.hasCS ? binding.shaderInfo.cs : shaders.cs;
	FormatShaderHash(vsHash, hasVS, vs, ARRAYSIZE(vs));
	FormatShaderHash(psHash, hasPS, ps, ARRAYSIZE(ps));
	FormatShaderHash(csHash, hasCS, cs, ARRAYSIZE(cs));
	FormatFileHash(filePath, hash, ARRAYSIZE(hash));

	const DX12DescriptorSummary &descriptor = binding.descriptor;

	char vsJson[64], psJson[64], csJson[64];
	char bindJson[64], kindJson[32], dimJson[32], fmtNameJson[64];
	char hashJson[32], fileJson[512], resourceJson[32];
	JsonEscapeString(vsJson, sizeof(vsJson), vs);
	JsonEscapeString(psJson, sizeof(psJson), ps);
	JsonEscapeString(csJson, sizeof(csJson), cs);
	JsonEscapeString(bindJson, sizeof(bindJson), binding.bindSpace.c_str());
	JsonEscapeString(kindJson, sizeof(kindJson), descriptor.kind.c_str());
	JsonEscapeString(dimJson, sizeof(dimJson), ResourceDimensionName(desc.Dimension));
	JsonEscapeString(fmtNameJson, sizeof(fmtNameJson), DxgiFormatName(static_cast<UINT>(desc.Format)));
	JsonEscapeString(hashJson, sizeof(hashJson), hash);
	JsonEscapeWString(fileJson, sizeof(fileJson), filePath);
	sprintf_s(resourceJson, sizeof(resourceJson), "\"%p\"", descriptor.resource);

	char buffer[2048];
	sprintf_s(buffer, sizeof(buffer),
		"{\"type\":\"bind.resource\","
		"\"event\":%llu,"
		"\"draw\":%llu,"
		"\"dispatch\":%llu,"
		"\"pso\":%llu,"
		"\"vs\":%s,"
		"\"ps\":%s,"
		"\"cs\":%s,"
		"\"bind\":%s,"
		"\"root\":%u,"
		"\"range\":%u,"
		"\"reg\":%u,"
		"\"space\":%u,"
		"\"desc\":%llu,"
		"\"kind\":%s,"
		"\"resource\":%s,"
		"\"dim\":%s,"
		"\"width\":%llu,"
		"\"height\":%u,"
		"fmt\":%u,"
		"fmt_name\":%s,"
		"\"view_dimension\":%u,"
		"\"first_element\":%llu,"
		"\"num_elements\":%u,"
		"\"structure_byte_stride\":%u,"
		"\"buffer_view_offset\":%llu,"
		"\"buffer_view_bytes\":%llu,"
		"\"gpu\":\"0x%llx\","
		"\"offset\":%llu,"
		"\"bytes\":%llu,"
		"\"state\":\"0x%x\","
		"\"state_known\":%s,"
		"\"file\":%s,"
		"\"hash\":%s}",
		static_cast<unsigned long long>(binding.eventSerial),
		static_cast<unsigned long long>(binding.drawId),
		static_cast<unsigned long long>(binding.dispatchId),
		static_cast<unsigned long long>(binding.psoIndex),
		vsJson,
		psJson,
		csJson,
		bindJson,
		binding.rootParameterIndex,
		binding.rangeIndex,
		binding.shaderRegister,
		binding.registerSpace,
		static_cast<unsigned long long>(binding.descriptorIndex),
		kindJson,
		resourceJson,
		dimJson,
		static_cast<unsigned long long>(desc.Width),
		desc.Height,
		static_cast<UINT>(desc.Format),
		fmtNameJson,
		descriptor.viewDimension,
		static_cast<unsigned long long>(descriptor.firstElement),
		descriptor.numElements,
		descriptor.structureByteStride,
		static_cast<unsigned long long>(descriptor.bufferViewOffset),
		static_cast<unsigned long long>(descriptor.bufferViewBytes),
		static_cast<unsigned long long>(descriptor.gpuVirtualAddress),
		static_cast<unsigned long long>(sourceOffset),
		static_cast<unsigned long long>(copyBytes),
		static_cast<UINT>(sourceState),
		hasCurrentState ? "true" : "false",
		fileJson,
		hashJson);

	DX12FrameAnalysisLogJsonLine(buffer);
}
