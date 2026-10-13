// Isolated regression stub: never executes filesystem requests or Shell verbs.
#include "../ipc/protocol.h"
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ipc;
    if (argc != 2) return 2;
    const DWORD parent_pid = wcstoul(argv[1], nullptr, 10);
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    if (!parent) return 3;
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto root = std::filesystem::path(module).parent_path();
    const bool generation = std::filesystem::exists(root / L"generation-mode");
    const bool partial = std::filesystem::exists(root / L"partial-mode");
    bool first_generation = false;
    HANDLE marker = CreateFileW((root / L"first-start.marker").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker != INVALID_HANDLE_VALUE) {
        const DWORD pid = GetCurrentProcessId(); DWORD written = 0;
        WriteFile(marker, &pid, sizeof(pid), &written, nullptr);
        CloseHandle(marker);
        if (!generation) { CloseHandle(parent); return 23; }
        first_generation = true;
    }
    if (!first_generation && GetLastError() != ERROR_FILE_EXISTS) { CloseHandle(parent); return 4; }
    if (!first_generation) {
    marker = CreateFileW((root / L"second-start.marker").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker == INVALID_HANDLE_VALUE) {
        marker = CreateFileW((root / L"third-start.marker").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
        CloseHandle(parent); return 5;
    }
    const DWORD pid = GetCurrentProcessId(); DWORD written = 0;
    WriteFile(marker, &pid, sizeof(pid), &written, nullptr); CloseHandle(marker);
    }
    DWORD written = 0;
    HANDLE pipe = CreateNamedPipeW(PipeNameFor(parent_pid).c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { CloseHandle(parent); return 6; }
    OVERLAPPED connect{}; connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = ConnectNamedPipe(pipe, &connect);
    const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    if (error == ERROR_PIPE_CONNECTED) connected = TRUE;
    else if (error == ERROR_IO_PENDING) {
        HANDLE waits[]{connect.hEvent, parent};
        if (WaitForMultipleObjects(2, waits, FALSE, 15000) == WAIT_OBJECT_0)
            connected = GetOverlappedResult(pipe, &connect, &written, FALSE);
        else {
            CancelIoEx(pipe, &connect);
            GetOverlappedResult(pipe, &connect, &written, TRUE);
        }
    }
    CloseHandle(connect.hEvent);
    if (connected) for (;;) {
        MsgHeader header{};
        if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
            header.magic != kMagic || header.payload_size > kMaxPayload) break;
        std::vector<uint8_t> payload(header.payload_size);
        if (!payload.empty() && !PipeRead(pipe, payload.data(), header.payload_size)) break;
        MsgHeader reply; reply.request_id = header.request_id;
        PayloadWriter response;
        if (header.type == REQ_PING) reply.type = RSP_PONG;
        else if (header.type == REQ_NEW_FILE) {
            if (generation && first_generation) {
                if (partial) {
                    MsgHeader incomplete; incomplete.type = RSP_DONE; incomplete.request_id = header.request_id;
                    if (!PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&incomplete), 8)) break;
                    HANDLE sent = CreateFileW((root / L"partial-sent.marker").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (sent != INVALID_HANDLE_VALUE) CloseHandle(sent);
                }
                continue; // Deliberately leave the first response incomplete.
            }
            // Acknowledge only: do not create the requested file.
            reply.type = RSP_DONE;
            response.PutU32(S_OK); response.PutU32(0); response.PutString(L"isolated stub ready");
        } else break;
        reply.payload_size = static_cast<uint32_t>(response.data().size());
        if (!PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)) ||
            (reply.payload_size && !PipeWrite(pipe, response.data().data(), reply.payload_size))) break;
    }
    CloseHandle(pipe); CloseHandle(parent);
    return connected ? 0 : 7;
}