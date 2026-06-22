#pragma once

#include <map>
#include <string>
#include <vector>

namespace Bunny {

struct IniEntry {
	std::wstring key;
	std::wstring value;
	std::wstring rawLine;
	int lineNumber = 0;
	bool hasEquals = false;
};

struct IniSection {
	std::wstring name;
	std::vector<IniEntry> entries;
};

class IniDocument {
public:
	bool LoadFromFile(const wchar_t *path);
	bool Parse(const wchar_t *text);

	const IniSection *FindSection(const wchar_t *section) const;
	const std::vector<IniSection> &Sections() const { return mSections; }

	bool GetString(const wchar_t *section, const wchar_t *key, std::wstring *value) const;
	bool GetBool(const wchar_t *section, const wchar_t *key, bool *value) const;
	bool GetInt(const wchar_t *section, const wchar_t *key, int *value) const;

	const std::wstring &Path() const { return mPath; }
	const std::wstring &Error() const { return mError; }

private:
	std::vector<IniSection> mSections;
	std::map<std::wstring, size_t> mSectionIndex;
	std::wstring mPath;
	std::wstring mError;
};

std::wstring ToLower(std::wstring value);
std::wstring Trim(const std::wstring &value);

} // namespace Bunny
