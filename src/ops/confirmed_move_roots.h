#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace pulse::ops {
// Elevated results may contain only descendants of a merged directory. Absence
// alone is not a completion: require an explicit root mapping and its target.
inline std::vector<std::wstring> ConfirmReportedMoveRoots(
    const std::vector<std::wstring>& roots, const std::vector<std::wstring>& sources,
    const std::vector<std::wstring>& destinations) {
    std::vector<std::wstring> confirmed;
    for (size_t i = 0; i < sources.size() && i < destinations.size(); ++i) {
        if (std::none_of(roots.begin(), roots.end(), [&](const auto& root) {
            return _wcsicmp(sources[i].c_str(), root.c_str()) == 0;
        })) continue;
        const DWORD attributes = GetFileAttributesW(sources[i].c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes == INVALID_FILE_ATTRIBUTES &&
            (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) &&
            GetFileAttributesW(destinations[i].c_str()) != INVALID_FILE_ATTRIBUTES)
            confirmed.push_back(sources[i]);
    }
    return confirmed;
}
} // namespace pulse::ops
