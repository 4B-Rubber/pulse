#include "../index/network_agent_protocol.h"
#include "../index/network_agent_security.h"
#include "../ipc/deadline_pipe.h"
#include <filesystem>
#include <iostream>
#include <vector>

int main() {
    using namespace pulse;
    namespace fs = std::filesystem;
    const auto token = L"deadline-" + std::to_wstring(GetCurrentProcessId());
    const auto root = fs::absolute(fs::path("bench_data") / token);
    fs::create_directories(root);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", root.c_str());
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto executable = fs::path(module).parent_path() / L"Pulse.Index.exe";
    std::wstring command = L"\"" + executable.wstring() + L"\" --test-network-agent " + token;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        std::cout << "[FAIL] isolated agent launch error=" << GetLastError() << '\n'; return 1;
    }
    CloseHandle(process.hThread);
    const auto pipe_name = index::agent::PipeName() + L".Test." + token;
    auto connect = [&](ULONGLONG limit) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        do {
            pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            Sleep(20);
        } while (GetTickCount64() < limit && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT);
        return pipe;
    };
    HANDLE silent = connect(GetTickCount64() + 10000);
    Sleep(100);
    const auto begin = GetTickCount64();
    HANDLE healthy = connect(begin + 8000);
    bool ok = silent != INVALID_HANDLE_VALUE && healthy != INVALID_HANDLE_VALUE;
    if (healthy != INVALID_HANDLE_VALUE) {
        auto request = index::agent::MakeHeader(index::agent::REQ_STATUS, 71, 0);
        ipc::MsgHeader response{}; const auto deadline = GetTickCount64() + 3000;
        const auto stopped = [] { return false; };
        ok = ok && index::agent::AuthorizeServer(healthy) &&
            ipc::DeadlinePipeIo(healthy, reinterpret_cast<uint8_t*>(&request), sizeof(request), true, deadline, stopped) &&
            ipc::DeadlinePipeIo(healthy, reinterpret_cast<uint8_t*>(&response), sizeof(response), false, deadline, stopped) &&
            response.magic == index::agent::kMagic && response.type == index::agent::RSP_STATUS &&
            response.request_id == 71 && response.payload_size > 0 && response.payload_size < 65536;
        if (ok) {
            std::vector<uint8_t> payload(response.payload_size);
            ok = ipc::DeadlinePipeIo(healthy, payload.data(), response.payload_size, false, deadline, stopped);
        }
        CloseHandle(healthy);
    }
    const auto elapsed = GetTickCount64() - begin;
    ok = ok && elapsed >= 4000 && elapsed < 8000;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << "actual isolated agent releases silent peer and serves authenticated next client; elapsed_ms=" << elapsed << '\n';
    if (silent != INVALID_HANDLE_VALUE) CloseHandle(silent);
    TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hProcess);
    fs::remove_all(root);
    return ok ? 0 : 1;
}
