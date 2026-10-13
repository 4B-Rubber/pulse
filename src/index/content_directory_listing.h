#pragma once
#include <windows.h>
#include <string>

namespace pulse::index::content_listing {
inline bool EmptyDirectoryResult(const std::wstring& directory, DWORD error) {
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES) return false;
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

#ifdef PULSE_CONTENT_ENUMERATION_TEST
// Test implementations delegate to Win32 and inject directory-specific failures.
HANDLE First(const std::wstring& query, WIN32_FIND_DATAW& data, DWORD flags);
BOOL Next(HANDLE handle, WIN32_FIND_DATAW& data);
void Close(HANDLE handle);
#else
inline HANDLE First(const std::wstring& query, WIN32_FIND_DATAW& data, DWORD flags) {
    return FindFirstFileExW(query.c_str(), FindExInfoBasic, &data,
        FindExSearchNameMatch, nullptr, flags);
}
inline BOOL Next(HANDLE handle, WIN32_FIND_DATAW& data) {
    return FindNextFileW(handle, &data);
}
inline void Close(HANDLE handle) {
    FindClose(handle);
}
#endif
} // namespace pulse::index::content_listing
