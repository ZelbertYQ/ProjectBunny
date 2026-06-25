#pragma once

#include <stdint.h>

#include <string>
#include <unordered_map>

#include "IniDocument.h"
#include "MigotoCommandList.h"

namespace Bunny {

struct ShaderOverrideConfig
{
	std::wstring section;
	std::wstring originalSection;
	std::wstring sourcePath;
	std::wstring sourceDir;
	std::wstring iniNamespace;
	uint64_t hash = 0;
	bool handlingSkip = false;
	CommandListLinks commandLists;
};

using ShaderOverrideMap = std::unordered_map<uint64_t, ShaderOverrideConfig>;

bool ParseShaderOverrideHash(const std::wstring &text, uint64_t *value);
void ParseShaderOverrideSections(
	const IniDocument &ini, ShaderOverrideMap *shaderOverrides);

} // namespace Bunny
