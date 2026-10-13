#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::fs {
struct RecycleMetadata {
    std::wstring original_path;
    uint64_t size = 0;
    FILETIME deleted{};
};
inline bool ParseRecycleMetadata(const BYTE* data, size_t bytes, RecycleMetadata& result) {
    result = {};
    if (!data || bytes < 24 || (bytes & 1)) return false;
    uint64_t version = 0;
    memcpy(&version, data, sizeof(version));
    size_t offset = 24, count = 260;
    if (version == 2) {
        if (bytes < 28) return false;
        uint32_t declared = 0;
        memcpy(&declared, data + 24, sizeof(declared));
        if (!declared || declared > 32768) return false;
        count = declared; offset = 28;
    } else if (version != 1) return false;
    if (count > (bytes - offset) / sizeof(wchar_t)) return false;
    std::wstring path(count, L'\0');
    memcpy(path.data(), data + offset, count * sizeof(wchar_t));
    const auto nul = path.find(L'\0');
    if (nul == std::wstring::npos || (version == 2 && nul != count - 1)) return false;
    path.resize(nul);
    if (path.empty()) return false;
    std::wstring core = path;
    if (core.starts_with(L"\\\\?\\UNC\\")) core = L"\\\\" + core.substr(8);
    else if (core.starts_with(L"\\\\?\\")) core.erase(0, 4);
    const bool drive = core.size() >= 3 && ((core[0] >= L'A' && core[0] <= L'Z') ||
        (core[0] >= L'a' && core[0] <= L'z')) && core[1] == L':' && core[2] == L'\\';
    const auto share = core.starts_with(L"\\\\") ? core.find(L'\\', 2) : std::wstring_view::npos;
    const bool unc = share != std::wstring_view::npos && share > 2 && share + 1 < core.size();
    if ((!drive && !unc) || path.back() == L'\\' || path.back() == L'/') return false;
    result.original_path = std::move(path);
    memcpy(&result.size, data + 8, 8);
    memcpy(&result.deleted, data + 16, 8);
    return true;
}
inline bool ReadRecycleMetadata(const std::wstring& path, RecycleMetadata& result) {
    result = {};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 24 || size.QuadPart > 28 + 32768 * 2) {
        CloseHandle(file); return false;
    }
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
    CloseHandle(file);
    return ok && read == bytes.size() && ParseRecycleMetadata(bytes.data(), read, result);
}
enum class RecycleRestoreResult { InvalidMetadata, MoveFailed, Restored };
// The caller validates ownership of the selected recycle payload. Moving must
// never replace a destination, and failed validation/moves preserve the index.
inline RecycleRestoreResult RestoreRecyclePayload(const std::wstring& index, const std::wstring& payload) {
    RecycleMetadata metadata;
    if (!ReadRecycleMetadata(index, metadata)) return RecycleRestoreResult::InvalidMetadata;
    if (!MoveFileExW(payload.c_str(), metadata.original_path.c_str(), 0)) return RecycleRestoreResult::MoveFailed;
    DeleteFileW(index.c_str());
    return RecycleRestoreResult::Restored;
}
} // namespace pulse::fs
