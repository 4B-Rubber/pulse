#include "preview_file_utils.h"
#include "../common/path_utils.h"
#include <cwctype>

namespace pulse::preview {

namespace {

constexpr DWORD kRecallOnOpen = 0x00040000; // FILE_ATTRIBUTE_RECALL_ON_OPEN
constexpr DWORD kRecallOnData = 0x00400000; // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
constexpr DWORD kPinned = 0x00080000; // FILE_ATTRIBUTE_PINNED

} // namespace

bool IsOfflinePlaceholder(DWORD attrs) {
    return (attrs & (kRecallOnOpen | kRecallOnData)) && !(attrs & kPinned);
}

std::wstring ExtensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring extension = path.substr(dot);
    for (wchar_t& c : extension) c = static_cast<wchar_t>(std::towlower(c));
    return extension;
}

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

} // namespace pulse::preview
