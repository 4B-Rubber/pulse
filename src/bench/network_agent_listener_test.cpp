#include "../index/network_agent_listener.h"
#include <cstdio>

namespace agent = pulse::index::agent;
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    const auto identity = agent::ProcessIdentity();
    agent::EndpointSecurity security(identity);
    const auto name = agent::PipeName(identity) + L"-listener-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    agent::PipeListener listener(name, security);
    check(listener.get() != INVALID_HANDLE_VALUE, "private first pipe instance created");
    if (listener.get() == INVALID_HANDLE_VALUE) return 1;
    const auto open = [&] {
        return CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    };
    const auto accept = [&] {
        return ConnectNamedPipe(listener.get(), nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
    };
    const auto takeover_denied = [&] {
        agent::PipeListener attacker(name, security);
        return attacker.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED;
    };
    HANDLE first = open();
    if (first == INVALID_HANDLE_VALUE) return 1;
    check(accept(), "first client accepted");
    HANDLE first_server = listener.TakeConnected();
    check(first_server != INVALID_HANDLE_VALUE, "next instance created while first client remains connected");
    if (first_server == INVALID_HANDLE_VALUE) { CloseHandle(first); return 1; }
    HANDLE second = open();
    check(second != INVALID_HANDLE_VALUE, "second legal client connects while first is still open");
    check(takeover_denied(), "first-instance takeover fails while clients are connected");
    const auto exchange = [&](HANDLE client, HANDLE server) {
        char sent = 'x', received = 0; DWORD bytes = 0;
        if (!agent::AuthorizedServer(client, identity) ||
            !WriteFile(client, &sent, 1, &bytes, nullptr) || bytes != 1 ||
            !ReadFile(server, &received, 1, &bytes, nullptr) || bytes != 1 ||
            !agent::AuthorizedClient(server, identity)) return false;
        auto other = identity; ++other.session;
        if (agent::AuthorizedClient(server, other) || agent::AuthorizedServer(client, other)) return false;
        return WriteFile(server, &received, 1, &bytes, nullptr) && bytes == 1 &&
            ReadFile(client, &received, 1, &bytes, nullptr) && bytes == 1 && received == sent;
    };
    check(exchange(first, first_server), "first client authorized and receives response; wrong session rejected");
    CloseHandle(first); CloseHandle(first_server);
    check(takeover_denied(), "endpoint remains reserved between requests");
    if (second != INVALID_HANDLE_VALUE) {
        check(accept(), "second client accepted after first request completes");
        HANDLE second_server = listener.TakeConnected();
        check(second_server != INVALID_HANDLE_VALUE && exchange(second, second_server),
            "second client authorized and receives response");
        CloseHandle(second);
        if (second_server != INVALID_HANDLE_VALUE) CloseHandle(second_server);
    }
    check(takeover_denied(), "idle listener still rejects endpoint takeover");
    std::printf("[INFO] Other-user/logon/session process not exercised; no accounts or sessions created.\n");
    return failures ? 1 : 0;
}
