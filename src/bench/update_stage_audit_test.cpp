#include "../app/update_stage.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <winioctl.h>
using namespace pulse::app;
static int failures;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
static bool Junction(const std::filesystem::path& path, const std::filesystem::path& target) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) return false;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct MountPoint {
        DWORD tag; WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[2048];
    } data{};
    const std::wstring name = L"\\??\\" + target.wstring();
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(name.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
    memcpy(data.path, name.c_str(), data.substitute_length + sizeof(wchar_t));
    DWORD bytes = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, DWORD(data.length) + 8,
        nullptr, 0, &bytes, nullptr) != FALSE;
    CloseHandle(handle); return ok;
}
static int RunningInstallerStage(const std::filesystem::path& root) {
    std::wstring directory, file;
    HANDLE lease = INVALID_HANDLE_VALUE;
    if (!CreateUpdateStage(root.wstring(), directory, file, lease)) {
        Check(false, "create isolated running-installer stage");
        return 1;
    }
    wchar_t self[32768]{};
    const std::wstring event_name = L"Local\\PulseStageAudit-" + std::to_wstring(GetCurrentProcessId());
    HANDLE exit_event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + file + L"\" --installer-fixture " + event_name;
    const bool started = exit_event && GetModuleFileNameW(nullptr, self, 32768) &&
        CopyFileW(self, file.c_str(), TRUE) && CreateProcessW(file.c_str(), command.data(),
            nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    CloseHandle(lease);
    Check(started, "start real owned executable from update stage");
    if (started) {
        SweepUpdateStages(root.wstring());
        Check(std::filesystem::exists(file) && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT,
            "startup cleanup preserves executing installer after downloader lease closes");
        SetEvent(exit_event);
        const auto wait = WaitForSingleObject(child.hProcess, 10000);
        DWORD code = 1;
        GetExitCodeProcess(child.hProcess, &code);
        Check(wait == WAIT_OBJECT_0 && code == 0, "owned installer exits normally");
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(child.hProcess, 2);
            WaitForSingleObject(child.hProcess, 5000);
        }
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        SweepUpdateStages(root.wstring());
        Check(!std::filesystem::exists(directory), "next cleanup removes exited installer payload and directory");
    }
    if (exit_event) CloseHandle(exit_event);
    return failures ? 1 : 0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && !wcscmp(argv[1], L"--installer-fixture")) {
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (!event) return 2;
        const auto wait = WaitForSingleObject(event, 15000);
        CloseHandle(event);
        return wait == WAIT_OBJECT_0 ? 0 : 3;
    }
    const auto root = std::filesystem::absolute(L"bench_data/update-stage-" + std::to_wstring(GetCurrentProcessId()));
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    if (argc == 2 && !wcscmp(argv[1], L"--process-only")) return RunningInstallerStage(root);
    std::wstring directory, file;
    HANDLE lease = INVALID_HANDLE_VALUE;
    Check(CreateUpdateStage(root.wstring(), directory, file, lease), "create marked isolated stage and exclusive lifetime lease");
    { std::ofstream output{std::filesystem::path(file)}; output << "installer fixture"; }
    SweepUpdateStages(root.wstring());
    Check(std::filesystem::exists(file), "live downloader lease prevents another instance cleanup");
    CloseHandle(lease);
    HANDLE executing = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    SweepUpdateStages(root.wstring());
    Check(std::filesystem::exists(file), "in-use installer remains intact after old process releases lease");
    CloseHandle(executing);
    SweepUpdateStages(root.wstring());
    Check(!std::filesystem::exists(directory), "later startup removes abandoned payload and its owned directory");
    const auto foreign = root / L"Pulse" / L"UpdateStaging" / std::wstring(32, L'a');
    std::filesystem::create_directories(foreign);
    { std::ofstream output(foreign / L"PulseSetup.exe"); output << "user data"; }
    SweepUpdateStages(root.wstring());
    Check(std::filesystem::exists(foreign / L"PulseSetup.exe"), "matching directory name without ownership marker is never removed");
    Check(CreateUpdateStage(root.wstring(), directory, file, lease), "create second isolated stage");
    { std::ofstream output(std::filesystem::path(directory) / L"notes.txt"); output << "user addition"; }
    CloseHandle(lease); SweepUpdateStages(root.wstring());
    Check(std::filesystem::exists(std::filesystem::path(directory) / L"notes.txt"), "unexpected user file prevents cleanup of whole stage");
    const auto target = root / L"outside";
    std::filesystem::create_directories(target);
    const auto link = root / L"Pulse" / L"UpdateStaging" / std::wstring(32, L'b');
    if (Junction(link, target)) {
        SweepUpdateStages(root.wstring());
        Check(std::filesystem::exists(target) && (GetFileAttributesW(link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "reparse stage is skipped without touching its target");
    } else Check(false, "create actual directory reparse fixture");
    return failures ? 1 : 0;
}
