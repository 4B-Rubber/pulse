#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>

namespace {
std::wstring fault_path;
int fault_mode = 0;
HANDLE fault_handle = INVALID_HANDLE_VALUE;
HANDLE WINAPI FaultFirst(LPCWSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data,
                         FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    std::wstring dir(pattern);
    dir.resize(dir.size() - 2);
    if (dir == fault_path) {
        if (fault_mode == 1) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
        if (fault_mode == 3 || fault_mode == 4) {
            if (fault_mode == 4) RemoveDirectoryW(dir.c_str());
            SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE;
        }
    }
    HANDLE handle = FindFirstFileExW(pattern, level, data, search, filter, flags);
    if (dir == fault_path && fault_mode == 2 && handle != INVALID_HANDLE_VALUE) {
        auto* entry = static_cast<WIN32_FIND_DATAW*>(data);
        while (!wcscmp(entry->cFileName, L".") || !wcscmp(entry->cFileName, L".."))
            if (!FindNextFileW(handle, entry)) break;
        fault_handle = handle;
    }
    return handle;
}
BOOL WINAPI FaultNext(HANDLE handle, LPWIN32_FIND_DATAW data) {
    if (handle == fault_handle) { SetLastError(ERROR_IO_DEVICE); return FALSE; }
    return FindNextFileW(handle, data);
}
BOOL WINAPI FaultClose(HANDLE handle) {
    if (handle == fault_handle) fault_handle = INVALID_HANDLE_VALUE;
    const BOOL ok = FindClose(handle);
    SetLastError(ERROR_INVALID_HANDLE); // Ensure the original enumeration error was captured.
    return ok;
}
}
#define FindFirstFileExW FaultFirst
#define FindNextFileW FaultNext
#define FindClose FaultClose
#include "../app/drop_staging.cpp"
#undef FindFirstFileExW
#undef FindNextFileW
#undef FindClose

int wmain() {
    namespace fs = std::filesystem;
    using namespace pulse::app;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    const auto parent = fs::absolute(L"bench_data");
    fs::create_directories(parent);
    const auto root = parent / (L"drop-stage-failure-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 1;
    const auto temp = root / L"temp", stages = temp / L"PulseDrop";
    const auto source = temp / L"extract", other = root / L"ordinary.txt";
    fs::create_directories(source / L"deep");
    std::ofstream(source / L"a.txt") << "a";
    std::ofstream(source / L"deep" / L"b.txt") << "b";
    std::ofstream(other) << "ordinary";
    const std::vector<std::wstring> sources{other.wstring(), (source / L"a.txt").wstring(), source.wstring()};
    for (int scenario = 0; scenario < 3; ++scenario) {
        fault_mode = scenario == 1 ? 2 : 1;
        fault_path = L"\\\\?\\" + (scenario == 2 ? source / L"deep" : source).wstring();
        std::vector<std::wstring> staged;
        auto result = StageDropSources(sources, temp.wstring(), stages.wstring(), staged);
        const DWORD expected_error = scenario == 1 ? ERROR_IO_DEVICE : ERROR_ACCESS_DENIED;
        check(result.error == expected_error,
            "first/middle/deep enumeration failure preserves original error");
        check(result.source == (scenario == 2 ? source / L"deep" : source).wstring(),
            "failure identifies original source directory");
        check(!result.any && staged == sources, "failed mixed-source batch publishes no partial paths");
        check(fs::is_empty(stages), "rollback removes previously staged prefix and incomplete tree");
        bool submitted = false;
        result = SubmitStagedDrop(sources, temp.wstring(), stages.wstring(),
            [&](const auto&) { submitted = true; });
        check(result.error && !submitted, "production Drop submission gate rejects incomplete COPY");
    }
    const auto empty = temp / L"empty";
    fs::create_directory(empty);
    fault_path = L"\\\\?\\" + empty.wstring();
    for (int mode : {0, 3, 4}) {
        fault_mode = mode;
        bool submitted = false;
        std::vector<std::wstring> published;
        const auto result = SubmitStagedDrop({empty.wstring()}, temp.wstring(), stages.wstring(),
            [&](const auto& paths) { submitted = true; published = paths; });
        if (mode == 4) check(result.error == ERROR_FILE_NOT_FOUND && !submitted,
            "disappeared directory is not accepted as an empty directory");
        else check(!result.error && submitted && published.front() != empty.wstring() &&
            fs::is_empty(published.front()), "real or FILE_NOT_FOUND empty directory stages successfully");
        SweepDropStages(stages.wstring(), true);
    }
    fault_mode = 0; fault_path.clear();
    bool ordinary_submitted = false;
    const auto ordinary = SubmitStagedDrop({other.wstring()}, temp.wstring(), stages.wstring(),
        [&](const auto& paths) { ordinary_submitted = paths == std::vector<std::wstring>{other.wstring()}; });
    check(!ordinary.error && !ordinary.any && ordinary_submitted, "non-temporary COPY passes through submission gate");
    check(fs::exists(source / L"a.txt") && fs::exists(source / L"deep" / L"b.txt") &&
        fs::exists(other), "rollback leaves original files intact");
    std::error_code error;
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"drop-stage-failure-"))
        fs::remove_all(root, error);
    check(!error && !fs::exists(root), "private fixture cleanup");
    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
