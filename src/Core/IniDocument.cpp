#include "IniDocument.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace Bunny {

std::wstring ToLower(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		[](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
	return value;
}

std::wstring Trim(const std::wstring &value)
{
	const wchar_t *spaces = L" \t\r\n";
	size_t first = value.find_first_not_of(spaces);
	if (first == std::wstring::npos)
		return L"";
	size_t last = value.find_last_not_of(spaces);
	return value.substr(first, last - first + 1);
}

static bool IsCommentOrEmpty(const std::wstring &line)
{
	std::wstring trimmed = Trim(line);
	return trimmed.empty() || trimmed[0] == L';' || trimmed[0] == L'#';
}

bool IniDocument::LoadFromFile(const wchar_t *path)
{
	mPath = path ? path : L"";
	mError.clear();

	std::wifstream file(mPath);
	if (!file.is_open()) {
		mError = L"Unable to open ini file";
		return false;
	}

	std::wstringstream buffer;
	buffer << file.rdbuf();
	return Parse(buffer.str().c_str());
}

bool IniDocument::Parse(const wchar_t *text)
{
	mSections.clear();
	mSectionIndex.clear();
	mError.clear();

	if (!text)
		return true;

	IniSection *current = nullptr;
	std::wistringstream stream(text);
	std::wstring line;
	int lineNumber = 0;

	while (std::getline(stream, line)) {
		lineNumber++;
		if (!line.empty() && line.back() == L'\r')
			line.pop_back();

		std::wstring trimmed = Trim(line);
		if (IsCommentOrEmpty(trimmed))
			continue;

		if (trimmed.size() >= 2 && trimmed.front() == L'[' && trimmed.back() == L']') {
			std::wstring sectionName = Trim(trimmed.substr(1, trimmed.size() - 2));
			std::wstring sectionKey = ToLower(sectionName);
			auto found = mSectionIndex.find(sectionKey);
			if (found == mSectionIndex.end()) {
				IniSection section;
				section.name = sectionName;
				mSections.push_back(section);
				mSectionIndex[sectionKey] = mSections.size() - 1;
				current = &mSections.back();
			} else {
				current = &mSections[found->second];
			}
			continue;
		}

		if (!current) {
			mError = L"Ini entry found before first section";
			return false;
		}

		IniEntry entry;
		entry.rawLine = line;
		entry.lineNumber = lineNumber;

		size_t equals = line.find(L'=');
		if (equals == std::wstring::npos) {
			entry.key = Trim(line);
			entry.hasEquals = false;
		} else {
			entry.key = Trim(line.substr(0, equals));
			entry.value = Trim(line.substr(equals + 1));
			entry.hasEquals = true;
		}
		current->entries.push_back(entry);
	}

	return true;
}

const IniSection *IniDocument::FindSection(const wchar_t *section) const
{
	if (!section)
		return nullptr;
	auto found = mSectionIndex.find(ToLower(section));
	if (found == mSectionIndex.end())
		return nullptr;
	return &mSections[found->second];
}

bool IniDocument::GetString(const wchar_t *section, const wchar_t *key, std::wstring *value) const
{
	const IniSection *iniSection = FindSection(section);
	if (!iniSection || !key)
		return false;

	std::wstring keyLower = ToLower(key);
	for (auto it = iniSection->entries.rbegin(); it != iniSection->entries.rend(); ++it) {
		if (!it->hasEquals)
			continue;
		if (ToLower(it->key) == keyLower) {
			if (value)
				*value = it->value;
			return true;
		}
	}
	return false;
}

bool IniDocument::GetBool(const wchar_t *section, const wchar_t *key, bool *value) const
{
	std::wstring text;
	if (!GetString(section, key, &text))
		return false;
	text = ToLower(Trim(text));
	bool parsed = text == L"1" || text == L"true" || text == L"yes" || text == L"on";
	if (!parsed && !(text == L"0" || text == L"false" || text == L"no" || text == L"off"))
		return false;
	if (value)
		*value = parsed;
	return true;
}

bool IniDocument::GetInt(const wchar_t *section, const wchar_t *key, int *value) const
{
	std::wstring text;
	if (!GetString(section, key, &text))
		return false;
	wchar_t *end = nullptr;
	long parsed = std::wcstol(text.c_str(), &end, 0);
	if (!end || *Trim(end).c_str())
		return false;
	if (value)
		*value = static_cast<int>(parsed);
	return true;
}

} // namespace Bunny
