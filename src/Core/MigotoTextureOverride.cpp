#include "MigotoTextureOverride.h"

namespace Bunny {

static bool StartsWithI(const std::wstring &value, const wchar_t *prefix)
{
	std::wstring lower = ToLower(value);
	std::wstring lowerPrefix = ToLower(prefix);
	return lower.rfind(lowerPrefix, 0) == 0;
}

bool ParseTextureOverrideHash(const std::wstring &text, uint32_t *value)
{
	if (!value)
		return false;

	std::wstring trimmed = Trim(text);
	if (trimmed.rfind(L"0x", 0) == 0 || trimmed.rfind(L"0X", 0) == 0)
		trimmed = trimmed.substr(2);
	if (trimmed.empty() || trimmed.size() > 8)
		return false;

	uint32_t parsed = 0;
	for (wchar_t ch : trimmed) {
		unsigned digit = 0;
		if (ch >= L'0' && ch <= L'9')
			digit = ch - L'0';
		else if (ch >= L'a' && ch <= L'f')
			digit = ch - L'a' + 10;
		else if (ch >= L'A' && ch <= L'F')
			digit = ch - L'A' + 10;
		else
			return false;
		parsed = (parsed << 4) | digit;
	}

	*value = parsed;
	return true;
}

void ParseTextureOverrideSections(
	const IniDocument &ini, TextureOverrideMap *textureOverrides)
{
	if (!textureOverrides)
		return;

	textureOverrides->clear();
	for (const IniSection &section : ini.Sections()) {
		if (!StartsWithI(section.name, L"TextureOverride"))
			continue;

		TextureOverrideConfig config;
		config.section = section.name;
		bool hasHash = false;

		for (const IniEntry &entry : section.entries) {
			std::wstring key = ToLower(entry.key);
			if (key == L"hash") {
				uint32_t hash = 0;
				if (!ParseTextureOverrideHash(entry.value, &hash))
					continue;
				config.hash = hash;
				hasHash = true;
				continue;
			}

			if (key == L"handling") {
				std::wstring value = ToLower(Trim(entry.value));
				if (value == L"skip")
					config.handlingSkip = true;
				continue;
			}
		}

		if (hasHash)
			(*textureOverrides)[config.hash] = config;
	}
}

} // namespace Bunny
