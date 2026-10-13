#include <windows.h>
#include <cstdio>
#include <string>
#include <filesystem>
#include <fstream>
static int failure_mode = 0;
static HANDLE child = nullptr;
static HANDLE job_copy = nullptr;
static HANDLE WINAPI TestCreateJob(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name) {
    if (failure_mode == 1) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    return CreateJobObjectW(attributes, name);
}
static BOOL WINAPI TestSetJob(HANDLE job, JOBOBJECTINFOCLASS kind, LPVOID value, DWORD bytes) {
    if (failure_mode == 2) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return SetInformationJobObject(job, kind, value, bytes);
}
static BOOL WINAPI TestAssign(HANDLE job, HANDLE process) {
    DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &child, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (failure_mode == 3) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    DuplicateHandle(GetCurrentProcess(), job, GetCurrentProcess(), &job_copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return AssignProcessToJobObject(job, process);
}
#define CreateJobObjectW TestCreateJob
#define SetInformationJobObject TestSetJob
#define AssignProcessToJobObject TestAssign
#include "../common/ffmpeg_tool.cpp"
#undef CreateJobObjectW
#undef SetInformationJobObject
#undef AssignProcessToJobObject

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--sleep") { Sleep(30000); return 0; }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--orphan-parent") {
        wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
        pulse::ffmpeg::Process process;
        if (!process.Start(self, L"--sleep", {})) return 2;
        { std::ofstream output{std::filesystem::path(argv[2])}; output << GetProcessId(child); }
        Sleep(30000);
        return 0;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    wchar_t self[32768]{};
    GetModuleFileNameW(nullptr, self, 32768);
    // Windows/CRT initialize persistent handles on the first process launch and output.
    printf("Warm process-launch infrastructure before counting per-launch resources.\n");
    {
        pulse::ffmpeg::Process warmup;
        pulse::ffmpeg::LaunchOptions options;
        if (!warmup.Start(self, L"--sleep", options)) return 2;
        if (job_copy) { CloseHandle(job_copy); job_copy = nullptr; }
    }
    if (child) { CloseHandle(child); child = nullptr; }
    for (int mode : {1, 2, 3, 0}) {
        failure_mode = mode;
        DWORD before = 0, after = 0;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        {
            pulse::ffmpeg::Process process;
            pulse::ffmpeg::LaunchOptions options;
            options.memory_limit = 128u * 1024 * 1024;
            const bool started = process.Start(self, L"--sleep", options);
            if (mode) {
                check(!started && !process.started() && GetLastError() == ERROR_ACCESS_DENIED,
                      "Job failure returns false with original error");
                check(!child || WaitForSingleObject(child, 1000) == WAIT_OBJECT_0,
                      "Job failure leaves no suspended or running child");
            } else {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                const bool queried = job_copy && QueryInformationJobObject(job_copy,
                    JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr);
                check(started && queried && limits.ProcessMemoryLimit == options.memory_limit &&
                    limits.BasicLimitInformation.ActiveProcessLimit == 1 &&
                    (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE),
                    "successful child has memory active-process and kill-on-close limits");
                if (job_copy) { CloseHandle(job_copy); job_copy = nullptr; }
            }
        }
        check(!child || WaitForSingleObject(child, 1000) == WAIT_OBJECT_0, "process destruction drains child");
        if (child) { CloseHandle(child); child = nullptr; }
        GetProcessHandleCount(GetCurrentProcess(), &after);
        printf("mode=%d handles before=%lu after=%lu\n", mode, before, after);
        check(after == before, "startup transaction releases all owned handles");
    }
    const auto fixture = std::filesystem::absolute(L"bench_data/ffmpeg-orphan-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    std::filesystem::create_directories(fixture.parent_path());
    auto command = pulse::ffmpeg::QuoteArgument(self) + L" --orphan-parent " + pulse::ffmpeg::QuoteArgument(fixture.wstring());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION parent{};
    const bool launched = CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &parent) != FALSE;
    DWORD child_id = 0;
    const auto deadline = GetTickCount64() + 4000;
    while (launched && !child_id && GetTickCount64() < deadline) {
        std::ifstream input(fixture); input >> child_id;
        if (!child_id) Sleep(10);
    }
    HANDLE orphan = child_id ? OpenProcess(SYNCHRONIZE, FALSE, child_id) : nullptr;
    check(launched && orphan && WaitForSingleObject(orphan, 0) == WAIT_TIMEOUT,
        "isolated parent has a live job-owned child before abrupt termination");
    if (launched) {
        TerminateProcess(parent.hProcess, 9);
        WaitForSingleObject(parent.hProcess, 2000);
        CloseHandle(parent.hThread); CloseHandle(parent.hProcess);
    }
    check(orphan && WaitForSingleObject(orphan, 2000) == WAIT_OBJECT_0,
        "abrupt parent exit kills child without running C++ destructors");
    if (orphan) CloseHandle(orphan);
    return failures ? 1 : 0;
}
