#pragma once
#ifdef PULSE_WITH_SELFTEST
#include "shell_tag_protocol.h"
#include <optional>

namespace pulse::app::shell_tags {
inline bool PrepareCommandAudit(int argc, wchar_t** argv, std::optional<CLSID>& audit_class) {
    bool isolated = false;
    const wchar_t* class_text = nullptr;
    const wchar_t* directory = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--test-instance") == 0) isolated = true;
        else if (wcscmp(argv[i], L"--tag-com-audit-class") == 0) {
            if (class_text || i + 1 >= argc) return false;
            class_text = argv[++i];
        } else if (wcscmp(argv[i], L"--tag-com-audit-dir") == 0) {
            if (directory || i + 1 >= argc) return false;
            directory = argv[++i];
        }
    }
    if (!class_text) return !directory;
    CLSID requested{}, production{};
    CLSIDFromString(kClassId, &production);
    if (!isolated || FAILED(CLSIDFromString(class_text, &requested)) ||
        requested == GUID_NULL || requested == production) return false;
    if (directory) {
        const std::wstring root(directory);
        const bool absolute = root.size() >= 3 && root[1] == L':' && (root[2] == L'\\' || root[2] == L'/');
        const auto profile = root + L"\\profile", temporary = root + L"\\tmp";
        const auto exists = [](const std::wstring& path) {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        };
        if (!absolute || !exists(root) || !exists(profile) || !exists(temporary)) return false;
        if (!SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile.c_str()) ||
            !SetEnvironmentVariableW(L"TEMP", temporary.c_str()) ||
            !SetEnvironmentVariableW(L"TMP", temporary.c_str())) return false;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) == 0) return false;
    audit_class = requested;
    return true;
}
}
#endif
