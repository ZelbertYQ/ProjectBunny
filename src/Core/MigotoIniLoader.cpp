#include "MigotoIniLoader.h"

#include <Windows.h>
#include <Shlwapi.h>

#include <algorithm>
#include <set>
#include <sstream>

namespace Bunny {

static bool StartsWithI(const std::wstring &value, const wchar_t *prefix)
{
	std::wstring lower = ToLower(value);
	std::wstring lowerPrefix = ToLower(prefix);
	return lower.rfind(lowerPrefix, 0) == 0;
}

static bool EndsWithI(const std::wstring &value, const wchar_t *suffix)
{
	std::wstring lower = ToLower(value);
	std::wstring lowerSuffix = ToLower(suffix);
	if (lower.size() < lowerSuffix.size())
		return false;
	return lower.compare(lower.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) == 0;
}

static bool IsAbsolutePath(const std::wstring &path)
{
	return path.size() >= 2 && (path[1] == L':' || path[0] == L'\\' || path[0] == L'/');
}

static std::wstring DirectoryOf(const std::wstring &path)
{
	wchar_t buffer[MAX_PATH];
	wcsncpy_s(buffer, path.c_str(), _TRUNCATE);
	PathRemoveFileSpecW(buffer);
	return buffer;
}

static std::wstring JoinPath(const std::wstring &base, const std::wstring &path)
{
	if (path.empty())
		return base;
	if (IsAbsolutePath(path))
		return path;

	wchar_t buffer[MAX_PATH];
	wcsncpy_s(buffer, base.c_str(), _TRUNCATE);
	PathAppendW(buffer, path.c_str());
	return buffer;
}

static std::wstring CanonicalPath(const std::wstring &path)
{
	wchar_t full[MAX_PATH];
	if (!GetFullPathNameW(path.c_str(), ARRAYSIZE(full), full, nullptr))
		return ToLower(path);
	return ToLower(full);
}

static bool IsDisabledName(const std::wstring &name)
{
	return StartsWithI(name, L"disabled") || StartsWithI(name, L"DISABLED");
}

static bool ShouldExcludeRecursive(const std::wstring &name, const std::vector<std::wstring> &patterns)
{
	if (IsDisabledName(name))
		return true;

	for (const std::wstring &pattern : patterns) {
		std::wstring lowerName = ToLower(name);
		std::wstring lowerPattern = ToLower(Trim(pattern));
		if (lowerPattern.empty())
			continue;
		if (lowerPattern == lowerName)
			return true;
		if (lowerPattern.back() == L'*') {
			lowerPattern.pop_back();
			if (lowerName.rfind(lowerPattern, 0) == 0)
				return true;
		}
	}
	return false;
}

static void AppendFileText(const std::wstring &path, std::wstringstream *combined)
{
	if (!combined)
		return;
	FILE *file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
	if (!file)
		return;

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size <= 0) {
		fclose(file);
		return;
	}

	std::string bytes;
	bytes.resize(static_cast<size_t>(size));
	size_t read = fread(&bytes[0], 1, bytes.size(), file);
	fclose(file);
	if (read != bytes.size())
		return;

	std::wstring text(bytes.begin(), bytes.end());
	*combined << L"\n; ---- begin include: " << path << L" ----\n";
	*combined << text;
	*combined << L"\n; ---- end include: " << path << L" ----\n";
}

struct LoaderState
{
	std::wstring rootDir;
	std::set<std::wstring> seenFiles;
	std::wstringstream combined;
	std::vector<std::wstring> loadedFiles;
	std::vector<std::wstring> warnings;
};

static void LoadFileRecursive(const std::wstring &path, LoaderState *state);

static void CollectIncludeEntries(
	const IniDocument &ini, std::vector<std::wstring> *includes,
	std::vector<std::wstring> *recursiveIncludes,
	std::vector<std::wstring> *excludeRecursive)
{
	for (const IniSection &section : ini.Sections()) {
		if (!StartsWithI(section.name, L"Include"))
			continue;
		for (const IniEntry &entry : section.entries) {
			if (!entry.hasEquals)
				continue;
			std::wstring key = ToLower(Trim(entry.key));
			if (key == L"include")
				includes->push_back(Trim(entry.value));
			else if (key == L"include_recursive")
				recursiveIncludes->push_back(Trim(entry.value));
			else if (key == L"exclude_recursive")
				excludeRecursive->push_back(Trim(entry.value));
		}
	}
}

static void LoadDirectoryRecursive(
	const std::wstring &dir, const std::vector<std::wstring> &excludeRecursive,
	LoaderState *state)
{
	if (!state)
		return;

	std::wstring search = JoinPath(dir, L"*");
	WIN32_FIND_DATAW findData = {};
	HANDLE find = FindFirstFileW(search.c_str(), &findData);
	if (find == INVALID_HANDLE_VALUE) {
		state->warnings.push_back(L"include_recursive path not found: " + dir);
		return;
	}

	std::vector<std::wstring> iniFiles;
	std::vector<std::wstring> directories;
	do {
		std::wstring name = findData.cFileName;
		if (name == L"." || name == L"..")
			continue;
		if (ShouldExcludeRecursive(name, excludeRecursive))
			continue;
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			directories.push_back(name);
		else if (EndsWithI(name, L".ini"))
			iniFiles.push_back(name);
	} while (FindNextFileW(find, &findData));
	FindClose(find);

	std::sort(iniFiles.begin(), iniFiles.end(),
		[](const std::wstring &a, const std::wstring &b) { return ToLower(a) < ToLower(b); });
	std::sort(directories.begin(), directories.end(),
		[](const std::wstring &a, const std::wstring &b) { return ToLower(a) < ToLower(b); });

	for (const std::wstring &file : iniFiles)
		LoadFileRecursive(JoinPath(dir, file), state);
	for (const std::wstring &child : directories)
		LoadDirectoryRecursive(JoinPath(dir, child), excludeRecursive, state);
}

static void LoadFileRecursive(const std::wstring &path, LoaderState *state)
{
	if (!state)
		return;

	std::wstring canonical = CanonicalPath(path);
	if (!state->seenFiles.insert(canonical).second) {
		state->warnings.push_back(L"ini included multiple times: " + path);
		return;
	}

	IniDocument ini;
	if (!ini.LoadFromFile(path.c_str())) {
		state->warnings.push_back(L"unable to load ini: " + path + L" (" + ini.Error() + L")");
		return;
	}

	state->loadedFiles.push_back(path);
	AppendFileText(path, &state->combined);

	std::vector<std::wstring> includes;
	std::vector<std::wstring> recursiveIncludes;
	std::vector<std::wstring> excludeRecursive;
	CollectIncludeEntries(ini, &includes, &recursiveIncludes, &excludeRecursive);

	std::wstring includeBase = DirectoryOf(path);
	for (const std::wstring &include : includes)
		LoadFileRecursive(JoinPath(includeBase, include), state);
	for (const std::wstring &recursive : recursiveIncludes)
		LoadDirectoryRecursive(JoinPath(includeBase, recursive), excludeRecursive, state);
}

bool LoadMigotoIniWithIncludes(const wchar_t *rootPath, MigotoIniLoadResult *result)
{
	if (!rootPath || !result)
		return false;

	*result = MigotoIniLoadResult();
	LoaderState state;
	state.rootDir = DirectoryOf(rootPath);
	LoadFileRecursive(rootPath, &state);

	result->loadedFiles = std::move(state.loadedFiles);
	result->warnings = std::move(state.warnings);
	return result->document.Parse(state.combined.str().c_str());
}

} // namespace Bunny
