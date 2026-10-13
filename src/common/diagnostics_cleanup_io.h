#pragma once
#include <windows.h>
#include <string>

namespace pulse::diagnostics {
// Per-call adapter: fault tests never replace process-wide filesystem functions.
struct CleanupIo {
    decltype(&GetFileAttributesW) attributes = GetFileAttributesW;
    decltype(&FindFirstFileW) first = FindFirstFileW;
    decltype(&FindNextFileW) next = FindNextFileW;
};
bool ClearCrashReportsWithIo(const std::wstring& root, std::wstring* error, const CleanupIo& io);
}
