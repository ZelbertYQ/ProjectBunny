#pragma once

#include <stdint.h>

#include <string>
#include <unordered_map>

#include "IniDocument.h"

namespace Bunny {

struct TextureOverrideConfig
{
	std::wstring section;
	uint32_t hash = 0;
	bool handlingSkip = false;
};

using TextureOverrideMap = std::unordered_map<uint32_t, TextureOverrideConfig>;

bool ParseTextureOverrideHash(const std::wstring &text, uint32_t *value);
void ParseTextureOverrideSections(
	const IniDocument &ini, TextureOverrideMap *textureOverrides);

} // namespace Bunny
