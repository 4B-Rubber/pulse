#include "../ipc/shell_client.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace pulse::ipc {
std::function<void()> review_pending_read;
void ObserveShellPendingReadForReview() { if (review_pending_read) review_pending_read(); }
}
namespace fs = std::filesystem;
DWORD MarkerPid(const fs::path& path) {
    DWORD pid = 0; std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(&pid), sizeof(pid)); return input ? pid : 0;
}
bool HasExited(DWORD pid) {
    if (!pid) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool ended = WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
    CloseHandle(process); return ended;
}
int Child(const fs::path& root) {
    auto& client = pulse::ipc::ShellClient::Instance();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl; failures += !ok;
    };
    std::atomic<unsigned> replies{0};
    pulse::ipc::ShellClient::Callbacks callbacks;
    callbacks.done = [&](uint32_t, uint32_t hr, bool cancelled, const std::wstring& text) {
        if (hr == S_OK && !cancelled && text == L"isolated stub ready") { ++replies; SetEvent(ready); }
    };
    client.Start(std::move(callbacks)); // Exactly one Start, no Stop/Start recovery.
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline && !fs::exists(root / L"second-start.marker")) Sleep(50);
    check(fs::exists(root / L"second-start.marker"),
          "M04-005 reader alone restarts the failed helper before any request is submitted");
    client.CreateNewFile((root / L"must-not-be-created.txt").wstring());
    WaitForSingleObject(ready, 3000);
    check(replies.load() > 0, "M04-005 client recovers after pre-connect child exit without Stop/Start");
    const DWORD first = MarkerPid(root / L"first-start.marker"), second = MarkerPid(root / L"second-start.marker");
    check(first && second && first != second && HasExited(first), "M04-005 failed first process retired and a distinct helper spawned");
    check(!fs::exists(root / L"must-not-be-created.txt"), "stub acknowledges only and never executes a filesystem operation");
    client.Stop();
    check(second ? HasExited(second) : HasExited(first), "isolated helper processes exited before fixture cleanup");
    CloseHandle(ready);
    return failures ? 1 : 0;
}
int GenerationChild(const fs::path& root) {
    auto& client = pulse::ipc::ShellClient::Instance();
    HANDLE blocked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE idle_read = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!blocked || !release || !ready || !idle_read) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << std::endl; failures += !ok;
    };
    std::atomic<bool> once{false};
    std::atomic<unsigned> timeouts{0};
    std::atomic<uint32_t> successful{0};
    std::atomic<unsigned> successful_replies{0};
    pulse::ipc::review_pending_read = [&] {
        if (once.exchange(true)) {
            if (successful.load()) SetEvent(idle_read);
            return;
        }
        SetEvent(blocked); WaitForSingleObject(release, 20000);
    };
    pulse::ipc::ShellClient::Callbacks callbacks;
    callbacks.done = [&](uint32_t id, uint32_t hr, bool cancelled, const std::wstring& text) {
        if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT))) ++timeouts;
        if (hr == S_OK && !cancelled && text == L"isolated stub ready") { successful = id; ++successful_replies; SetEvent(ready); }
    };
    client.Start(std::move(callbacks));
    check(WaitForSingleObject(blocked, 10000) == WAIT_OBJECT_0,
          "M04-004 old reader held with an outstanding OVERLAPPED read");
    const auto first = client.CreateNewFile((root / L"first-noop.txt").wstring());
    if (fs::exists(root / L"partial-mode")) {
        const auto deadline = GetTickCount64() + 3000;
        while (!fs::exists(root / L"partial-sent.marker") && GetTickCount64() < deadline) Sleep(10);
        check(fs::exists(root / L"partial-sent.marker"), "M04-004 old read contains only a partial response header before retirement");
    }
    client.Abort(first);
    Sleep(1700); // Preserve the production 1500ms spawn throttle, not a new lifecycle.
    const auto second = client.CreateNewFile((root / L"second-noop.txt").wstring());
    const DWORD second_pid = MarkerPid(root / L"second-start.marker");
    HANDLE second_process = second_pid ? OpenProcess(SYNCHRONIZE, FALSE, second_pid) : nullptr;
    check(first && second && first != second && second_process &&
          WaitForSingleObject(second_process, 0) == WAIT_TIMEOUT,
          "M04-004 replacement helper accepts the new request before old reader is released");
    SetEvent(release);
    check(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0 && successful.load() == second && successful_replies.load() == 1,
          "M04-004 stale read failure cannot discard the replacement connection request");
    check(timeouts.load() == 1 && second_process && WaitForSingleObject(second_process, 0) == WAIT_TIMEOUT &&
          !fs::exists(root / L"third-start.marker"),
          "M04-004 original Abort completes once and old failure never kills or restarts the new helper");
    client.Abort(first); client.Abort(0xffffffffu);
    check(second_process && WaitForSingleObject(second_process, 150) == WAIT_TIMEOUT && timeouts.load() == 1,
          "M04-004 stale and unknown Abort IDs cannot retire a replacement helper");
    check(WaitForSingleObject(idle_read, 10000) == WAIT_OBJECT_0,
          "M04-004 replacement reader has an outstanding idle read before Stop");
    const auto stopping = GetTickCount64();
    client.Stop(); pulse::ipc::review_pending_read = {};
    check(GetTickCount64() - stopping < 3000, "M04-004 Stop cancels and drains the isolated pending read within the test bound");
    check(second_process && WaitForSingleObject(second_process, 5000) == WAIT_OBJECT_0 &&
          !fs::exists(root / L"first-noop.txt") && !fs::exists(root / L"second-noop.txt"),
          "M04-004 isolated helpers shut down without executing filesystem requests");
    if (second_process) CloseHandle(second_process);
    CloseHandle(blocked); CloseHandle(release); CloseHandle(ready); CloseHandle(idle_read);
    return failures ? 1 : 0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--child") return fs::exists(fs::path(argv[2]) / L"generation-mode") ? GenerationChild(fs::path(argv[2])) : Child(fs::path(argv[2]));
    const bool partial = argc == 2 && std::wstring(argv[1]) == L"--partial-generation";
    const bool generation = partial || (argc == 2 && std::wstring(argv[1]) == L"--generation");
    if (argc != 1 && !generation) return 2;
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const fs::path self(module);
    const auto root = fs::absolute(fs::path("bench_data") /
        ("shell-reconnect-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    int result = 2;
    try {
        if (!fs::create_directories(root)) return 2;
        if (generation) std::ofstream(root / L"generation-mode") << "isolated generation regression";
        if (partial) std::ofstream(root / L"partial-mode") << "partial response header";
        fs::copy_file(self, root / L"client.exe");
        fs::copy_file(self.parent_path() / L"pulse_shell_reconnect_stub.exe", root / L"pulse_shell.exe");
        std::wstring command = L"\"" + (root / L"client.exe").wstring() + L"\" --child \"" + root.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, root.c_str(), &startup, &process)) {
            if (WaitForSingleObject(process.hProcess, 25000) != WAIT_OBJECT_0) {
                std::cout << "[FAIL] bounded isolated client regression timeout" << std::endl;
                TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                WaitForSingleObject(process.hProcess, 5000);
            } else {
                DWORD code = 2; GetExitCodeProcess(process.hProcess, &code); result = static_cast<int>(code);
            }
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        } else std::cout << "[FAIL] isolated client launch" << std::endl;
        const DWORD first = MarkerPid(root / L"first-start.marker"), second = MarkerPid(root / L"second-start.marker");
        if (first) HasExited(first);
        if (second) HasExited(second);
        std::error_code error; fs::remove_all(root, error);
        const bool clean = !error && !fs::exists(root);
        std::cout << (clean ? "[PASS] " : "[FAIL] ") << "only isolated copied client/stub fixture cleaned up" << std::endl;
        if (!clean) result = 1;
    } catch (const std::exception& error) {
        result = 1;
        std::cout << "[FAIL] " << error.what() << std::endl;
    }
    return result;
}