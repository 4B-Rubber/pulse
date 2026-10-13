#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include "../common/crash_reporter.h"

namespace {
int fault = 0;
DWORD WINAPI TestAttributes(LPCWSTR path) {
    if (fault == 1) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_FILE_ATTRIBUTES; }
    if (fault == 4) return FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    return GetFileAttributesW(path);
}
HANDLE WINAPI TestFirst(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    if (fault == 2) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    return FindFirstFileW(path, data);
}
BOOL WINAPI TestNext(HANDLE handle, LPWIN32_FIND_DATAW data) {
    if (fault == 3) { SetLastError(ERROR_READ_FAULT); return FALSE; }
    return FindNextFileW(handle, data);
}
}
#define GetFileAttributesW TestAttributes
#define FindFirstFileW TestFirst
#define FindNextFileW TestNext
#include "../common/diagnostics_exporter.cpp"
#undef GetFileAttributesW
#undef FindFirstFileW
#undef FindNextFileW

namespace {
int failures = 0;
void Check(bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
size_t Count(const std::filesystem::path& dir) {
    size_t n = 0;
    for (const auto& file : std::filesystem::directory_iterator(dir)) if (file.path().extension() == L".json") ++n;
    return n;
}
void Inject() {
    __try { RaiseException(0xE0424242, 0, 0, nullptr); }
    __except(pulse::crash::ReportRecoverable(GetExceptionInformation(), "retention-test")) {}
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2) {
        if (!pulse::crash::Initialize({pulse::crash::ProcessRole::Test, false, argv[1]})) return 2;
        for (int i = 0; i < 14; ++i) Inject();
        pulse::crash::Shutdown();
        return 0;
    }
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const auto root = std::filesystem::path(temp) / (L"PulseM02-" + std::to_wstring(GetCurrentProcessId()));
    const auto dir = root / L"Diagnostics" / L"Crashes";
    std::wstring error;
    Check(pulse::diagnostics::ClearCrashReports(root.wstring(), &error), "missing directory succeeds");
    std::filesystem::create_directories(dir);
    Check(pulse::diagnostics::ClearCrashReports(root.wstring(), &error), "empty directory succeeds");
    for (int i = 1; i <= 4; ++i) {
        fault = i; error.clear();
        Check(!pulse::diagnostics::ClearCrashReports(root.wstring(), &error) && !error.empty(), "attribute/enumeration/reparse failure is reported");
    }
    fault = 0;
    const auto locked = dir / L"blocked.json";
    HANDLE file = CreateFileW(locked.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE && !pulse::diagnostics::ClearCrashReports(root.wstring(), &error) && !error.empty(), "actual Windows sharing violation is reported");
    CloseHandle(file);
    Check(pulse::diagnostics::ClearCrashReports(root.wstring(), &error) && error.empty(), "unlocked cleanup succeeds and clears error");
    Check(pulse::crash::Initialize({pulse::crash::ProcessRole::Test, false, root.wstring()}), "initialize retention worker");
    for (int i = 0; i < 14; ++i) Inject();
    const auto deadline = GetTickCount64() + 5000;
    while (Count(dir) > 10 && GetTickCount64() < deadline) Sleep(50);
    Check(Count(dir) == 10, "fourteen recoverable SEH reports retained to ten without restart");
    size_t dumps = 0; uint64_t bytes = 0; bool newest = false;
    for (const auto& item : std::filesystem::directory_iterator(dir)) {
        bytes += item.file_size();
        if (item.path().extension() == L".dmp") ++dumps;
        if (item.path().filename().wstring().ends_with(L"-14.json")) newest = true;
    }
    printf("Retained JSON=%zu dumps=%zu bytes=%llu\n", Count(dir), dumps, static_cast<unsigned long long>(bytes));
    Check(dumps == 10 && newest && bytes <= 200ull * 1024 * 1024, "complete event pairs, newest event and byte budget");
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    std::wstring command = L"\"" + std::wstring(self) + L"\" \"" + root.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
    Check(started, "start concurrent reporting process");
    for (int i = 0; i < 14; ++i) Inject();
    if (started) {
        const DWORD waited = WaitForSingleObject(child.hProcess, 30000);
        DWORD code = 1; GetExitCodeProcess(child.hProcess, &code);
        Check(waited == WAIT_OBJECT_0 && code == 0, "concurrent reporter exits successfully");
        if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 4);
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }
    Sleep(1500);
    bool paired = true;
    for (const auto& item : std::filesystem::directory_iterator(dir)) {
        if (item.path().extension() == L".json") {
            auto dump = item.path(); dump.replace_extension(L".dmp");
            paired &= std::filesystem::exists(dump);
        }
        if (item.path().extension() == L".dmp") {
            auto json = item.path(); json.replace_extension(L".json");
            paired &= std::filesystem::exists(json);
        }
    }
    Check(Count(dir) <= 10 && paired, "concurrent reporting retains complete groups");
    const auto oversized = dir / L"old-locked.json";
    file = CreateFileW(oversized.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
    LARGE_INTEGER size{}; size.QuadPart = 210ll * 1024 * 1024;
    bool sized = file != INVALID_HANDLE_VALUE && SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    FILETIME old{1, 0}; if (file != INVALID_HANDLE_VALUE) SetFileTime(file, nullptr, nullptr, &old);
    Sleep(1500);
    Check(sized && std::filesystem::exists(oversized) && Count(dir) == 2,
        "failed deletion keeps bytes charged and removes other old events");
    CloseHandle(file);
    Sleep(1500);
    Check(!std::filesystem::exists(oversized) && Count(dir) == 1,
        "periodic retry releases oversized locked event after unlock");
    pulse::crash::Shutdown();
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
