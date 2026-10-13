#include "../index/index_protocol.h"
#include "../index/index_service_start.h"
#include "../ipc/protocol.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace pulse::index;
using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* name) {
    std::wcout << (condition ? L"[PASS] " : L"[FAIL] ") << name << L'\n';
    if (!condition) ++failures;
}

std::wstring SiblingExecutable(const wchar_t* name) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return name;
    return std::wstring(path, slash + 1) + name;
}

HANDLE Connect(const std::wstring& pipe_name, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        const DWORD failure = GetLastError();
        if (GetTickCount64() >= deadline) {
            std::wcout << L"[INFO] pipe connect error " << failure << L"\n";
            return INVALID_HANDLE_VALUE;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(pipe_name.c_str(), 50);
        else Sleep(10);
    }
}

bool SendFrame(HANDLE pipe, uint32_t type, uint32_t id,
               const std::vector<uint8_t>& payload = {}) {
    const MsgHeader header = MakeIndexHdr(type, id, static_cast<uint32_t>(payload.size()));
    return PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) &&
        (payload.empty() || PipeWrite(pipe, payload.data(),
                                      static_cast<DWORD>(payload.size())));
}

bool ReadFrame(HANDLE pipe, MsgHeader& header, std::vector<uint8_t>& payload,
               DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available >= sizeof(header)) break;
        if (GetTickCount64() >= deadline) return false;
        Sleep(2);
    }
    if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
        header.magic != kIndexMagic || header.payload_size > kIndexMaxPayload)
        return false;
    payload.resize(header.payload_size);
    return payload.empty() || PipeRead(pipe, payload.data(), header.payload_size);
}

std::vector<uint8_t> SearchPayload(std::wstring needle) {
    PayloadWriter writer;
    writer.PutU32(1u);
    writer.PutU32(static_cast<uint32_t>(ResultSort::Index));
    writer.PutU32(48);
    writer.PutU32(0);
    writer.PutString(needle);
    writer.PutString(L"");
    return writer.data();
}

bool WaitForSearch(HANDLE pipe, uint32_t wanted_id, DWORD timeout_ms,
                   uint32_t* responses = nullptr, uint32_t* matched = nullptr) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    uint32_t count = 0;
    while (GetTickCount64() < deadline) {
        MsgHeader header{};
        std::vector<uint8_t> payload;
        const DWORD remaining = static_cast<DWORD>((std::min<ULONGLONG>)(
            deadline - GetTickCount64(), 250));
        if (!ReadFrame(pipe, header, payload, remaining)) continue;
        if (header.type != RSP_IDX_SEARCH) continue;
        ++count;
        if (header.request_id == wanted_id) {
            PayloadReader reader(payload.data(), payload.size());
            uint32_t total = 0, hits = 0;
            const bool valid = reader.GetU32(total) && reader.GetU32(hits) && hits <= 48;
            if (responses) *responses = count;
            if (matched) *matched = total;
            return valid;
        }
    }
    if (responses) *responses = count;
    return false;
}

} // namespace

int wmain() {
    std::wcout << std::unitbuf;
    const auto token = std::to_wstring(GetCurrentProcessId()) + L"-backpressure-" + std::to_wstring(GetTickCount64());
    const auto name = L"\\\\.\\pipe\\PulseIndex.Test." + token;
    const auto exe = SiblingExecutable(L"Pulse.Index.exe");
    std::wstring command = L"\"" + exe + L"\" --test-host " + token;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 1;
    CloseHandle(process.hThread);
    HANDLE slow = Connect(name, 15000);
    HANDLE normal = Connect(name, 3000);
    bool ready = true;
    for (HANDLE pipe : {slow, normal}) {
        MsgHeader header{}; std::vector<uint8_t> payload;
        ready = pipe != INVALID_HANDLE_VALUE && ReadFrame(pipe, header, payload, 2000) && ready;
    }
    Check(ready, L"isolated host accepts slow and normal clients");
    if (ready) {
        PayloadWriter large;
        large.PutU32(0); large.PutU32(static_cast<uint32_t>(ResultSort::Index));
        large.PutU32(4096); large.PutU32(0); large.PutString(L"stress"); large.PutString(L"");
        Check(SendFrame(slow, REQ_IDX_SEARCH, 1, large.data()), L"large response requested without reading");
        DWORD available = 0;
        const auto pending_until = GetTickCount64() + 2000;
        while (GetTickCount64() < pending_until && available < 65536) {
            if (!PeekNamedPipe(slow, nullptr, 0, nullptr, &available, nullptr)) break;
            Sleep(2);
        }
        Check(available >= 65536, L"slow client fills the server output buffer");
        const auto start = GetTickCount64();
        Check(SendFrame(normal, REQ_IDX_SEARCH, 2, SearchPayload(L"stress")) &&
            WaitForSearch(normal, 2, 1500), L"normal search completes while slow response remains blocked");
        std::wcout << L"[INFO] normal query latency " << GetTickCount64() - start << L" ms\n";
        HWND window = nullptr;
        for (HWND candidate = FindWindowExW(HWND_MESSAGE, nullptr, L"PulseIndexHost", nullptr);
             candidate; candidate = FindWindowExW(HWND_MESSAGE, candidate, L"PulseIndexHost", nullptr)) {
            DWORD pid = 0; GetWindowThreadProcessId(candidate, &pid);
            if (pid == process.dwProcessId) { window = candidate; break; }
        }
        bool broadcast = window && PostMessageW(window, WM_APP + 1, 0, 0);
        MsgHeader status{}; std::vector<uint8_t> payload;
        broadcast = broadcast && ReadFrame(normal, status, payload, 1000) && status.type == RSP_IDX_STATUS;
        Check(broadcast, L"window status broadcast reaches normal client during blocked write");
        Sleep(3300);
        Check(!SendFrame(slow, REQ_IDX_STATUS, 3), L"total write deadline disconnects non-reading client");
        CloseHandle(slow);
        slow = Connect(name, 3000);
        MsgHeader initial{}; std::vector<uint8_t> initial_payload;
        const bool again = slow != INVALID_HANDLE_VALUE && ReadFrame(slow, initial, initial_payload, 1000) &&
            SendFrame(slow, REQ_IDX_SEARCH, 4, large.data());
        Sleep(100);
        Check(again && window && PostMessageW(window, WM_APP + 2, 0, 0), L"shutdown requested during another blocked write");
    }
    const auto stopped = WaitForSingleObject(process.hProcess, 5000);
    Check(stopped == WAIT_OBJECT_0, L"host joins reader and writer threads during shutdown");
    if (stopped != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
    for (HANDLE pipe : {slow, normal}) if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    CloseHandle(process.hProcess);
    return failures ? 1 : 0;
}
