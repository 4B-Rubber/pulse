#pragma once
// Where Pulse keeps per-user data. Shared so the app stores and the preview cache agree
// on the location, including the self-test redirect; the light preview test links this
// header without pulling the app module in.
#include <string>
#include <windows.h>
#include <shlobj.h>

namespace pulse {

inline std::wstring PulseDataDir() {
#ifdef PULSE_WITH_SELFTEST
    wchar_t test_dir[32768]{};
    const DWORD length =
        GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", test_dir, ARRAYSIZE(test_dir));
    if (length > 0 && length < ARRAYSIZE(test_dir)) {
        CreateDirectoryW(test_dir, nullptr);
        return test_dir;
    }
#endif
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        std::wstring dir = std::wstring(path) + L"\\Pulse";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return L"";
}

} // namespace pulse
