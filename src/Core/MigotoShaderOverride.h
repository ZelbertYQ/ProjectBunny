#pragma once

#include <stdint.h>

#include <string>
#include <unordered_map>

#include "IniDocument.h"

namespace Bunny {

struct ShaderOverrideConfig
{
	std::wstring section;
	uint64_t hash = 0;
	bool handlingSkip = false;
};

using ShaderOverrideMap = std::unordered_map<uint64_t, ShaderOverrideConfig>;

bool ParseShaderOverrideHash(const std::wstring &text, uint64_t *value);
void ParseShaderOverrideSections(
	const IniDocument &ini, ShaderOverrideMap *shaderOverrides);

} // namespace Bunny
