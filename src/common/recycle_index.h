#pragma once

#include "path_utils.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#ifdef PULSE_RECYCLE_METADATA_TEST
namespace pulse::review { DWORD RecycleReadAmountForReview(DWORD requested); }
#endif

namespace pulse::recycle {

struct IndexRecord {
    std::wstring original_path;
    uint64_t size = 0;
    FILETIME deleted{};
};
inline constexpr size_t kMaxPathUnits = 32768;
inline constexpr size_t kMaxIndexBytes = 28 + kMaxPathUnits * 2;

inline uint64_t ReadLittleEndian(const BYTE* bytes, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= uint64_t{bytes[i]} << (8 * i);
    return value;
}

inline bool SupportedAbsolutePath(std::wstring_view original) {
    // Validate the exact conversion used by the restore caller. In particular,
    // unknown device namespaces must not become relative paths after stripping.
    const std::wstring path = pulse::path::StripExtendedPathPrefix(original);
    const auto sep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    if (path.size() > 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
                           (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && sep(path[2])) return true;
    if (path.size() < 5 || !sep(path[0]) || !sep(path[1])) return false;
    const size_t server_end = path.find_first_of(L"\\/", 2);
    if (server_end == std::wstring::npos || server_end == 2) return false;
    const auto server = std::wstring_view(path).substr(2, server_end - 2);
    if (server == L"." || server == L"?" || server == L"..") return false;
    const size_t share_end = path.find_first_of(L"\\/", server_end + 1);
    return share_end != std::wstring::npos && share_end > server_end + 1 &&
           share_end + 1 < path.size();
}

inline bool ParseIndex(const BYTE* data, size_t bytes, IndexRecord& out) {
    static_assert(sizeof(wchar_t) == 2, "Recycle metadata uses Windows UTF-16");
    out = {};
    if (!data || bytes < 24 || bytes > kMaxIndexBytes) return false;
    const uint64_t version = ReadLittleEndian(data, 8);
    size_t offset = 0, units = 0;
    if (version == 1) {
        // v1 has a fixed 260-WCHAR filename field, not a variable short record.
        if (bytes != 24 + 260 * 2) return false;
        offset = 24; units = 260;
    } else if (version == 2) {
        if (bytes < 28) return false;
        units = static_cast<size_t>(ReadLittleEndian(data + 24, 4));
        if (units < 2 || units > kMaxPathUnits || bytes != 28 + units * 2) return false;
        offset = 28;
    } else return false;

    std::wstring path;
    path.reserve(units);
    bool terminated = false;
    for (size_t i = 0; i < units; ++i) {
        const wchar_t ch = static_cast<wchar_t>(ReadLittleEndian(data + offset + i * 2, 2));
        if (ch == L'\0') {
            // v1's bytes after its terminator are fixed-field padding. In v2,
            // the declared length must describe one complete string incl. NUL.
            if (version == 2 && i + 1 != units) return false;
            terminated = true;
            break;
        }
        path.push_back(ch);
    }
    if (!terminated || path.empty() || !SupportedAbsolutePath(path)) return false;
    IndexRecord parsed;
    parsed.original_path = std::move(path);
    parsed.size = ReadLittleEndian(data + 8, 8);
    parsed.deleted.dwLowDateTime = static_cast<DWORD>(ReadLittleEndian(data + 16, 4));
    parsed.deleted.dwHighDateTime = static_cast<DWORD>(ReadLittleEndian(data + 20, 4));
    out = std::move(parsed);
    return true;
}

inline bool ReadIndex(const std::wstring& path, IndexRecord& out) {
    out = {};
    struct FileOwner {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~FileOwner() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        FileOwner(const FileOwner&) = delete;
        FileOwner& operator=(const FileOwner&) = delete;
        explicit FileOwner(HANDLE value) : handle(value) {}
    } file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < 24 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxIndexBytes) return false;
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
    DWORD requested = static_cast<DWORD>(bytes.size());
#ifdef PULSE_RECYCLE_METADATA_TEST
    requested = pulse::review::RecycleReadAmountForReview(requested);
    if (requested > bytes.size()) return false;
#endif
    DWORD received = 0;
    if (!ReadFile(file.handle, bytes.data(), requested, &received, nullptr) ||
        received != bytes.size()) return false;
    return ParseIndex(bytes.data(), received, out);
}

} // namespace pulse::recycle
