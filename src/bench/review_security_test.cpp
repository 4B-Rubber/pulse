#include "../index/network_agent_security.h"
#include "../index/index_directory_security.h"
#include "../index/index_migration.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>

namespace fs = std::filesystem;
namespace agent = pulse::index::agent;
namespace security = pulse::index::security;

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    const auto identity = agent::ProcessIdentity();
    check(identity.valid(), "M06 current token provides user/logon/session identity");
    auto other = identity; ++other.session;
    check(!agent::SameIdentity(identity, other) && agent::PipeName(identity) != agent::PipeName(other),
        "M06 different sessions have distinct endpoints and fail authorization");
    other = identity; other.user = L"S-1-5-21-1-2-3-4567";
    check(!agent::SameIdentity(identity, other) && agent::SingletonName(identity) != agent::SingletonName(other),
        "M06 different users have distinct singleton and authorization");
    other = identity; other.logon = L"S-1-5-5-1-2";
    check(!agent::SameIdentity(identity, other) && agent::PipeName(identity) != agent::PipeName(other),
        "M06 same-account different logons remain isolated");
    agent::EndpointSecurity invalid(agent::Identity{}), endpoint(identity);
    check(!invalid && endpoint && agent::PipeName({}).empty(), "M06 identity/security failure is fail-closed");

    HANDLE primary = nullptr, impersonation = nullptr, restricted = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &primary);
    if (primary) DuplicateToken(primary, SecurityImpersonation, &impersonation);
    // A restricting SID absent from the logon-only DACL must not gain access.
    BYTE world[SECURITY_MAX_SID_SIZE]{}; DWORD world_size = sizeof(world);
    CreateWellKnownSid(WinWorldSid, nullptr, world, &world_size);
    SID_AND_ATTRIBUTES restriction{world, 0};
    if (primary && !CreateRestrictedToken(primary, 0, 0, nullptr, 0, nullptr, 1, &restriction, &restricted))
        std::printf("[INFO] CreateRestrictedToken error: %lu\n", GetLastError());
    check(impersonation && restricted, "M06 restricted impersonation token fixture created");

    const auto pipe_name = agent::PipeName(identity) + L"-fixture-" + std::to_wstring(GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 0, endpoint.get());
    check(server != INVALID_HANDLE_VALUE, "M06 creates isolated local-only named pipe with explicit DACL");
    if (server != INVALID_HANDLE_VALUE) {
        const bool impersonated = restricted && ImpersonateLoggedOnUser(restricted);
        HANDLE denied = INVALID_HANDLE_VALUE;
        DWORD denied_error = 0;
        if (impersonated) {
            denied = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            denied_error = GetLastError();
            if (!RevertToSelf()) TerminateProcess(GetCurrentProcess(), ERROR_ACCESS_DENIED);
        }
        check(impersonated && denied == INVALID_HANDLE_VALUE && denied_error == ERROR_ACCESS_DENIED,
            "M06 actual Windows pipe DACL rejects a restricted client token");
        if (denied != INVALID_HANDLE_VALUE) { CloseHandle(denied); DisconnectNamedPipe(server); }
        std::atomic<bool> correct{false}, wrong{true};
        std::thread worker([&] {
            if (ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
                char byte = 0; DWORD count = 0;
                if (ReadFile(server, &byte, 1, &count, nullptr) && count == 1) {
                    correct = agent::AuthorizedClient(server, identity);
                    auto wrong_owner = identity; ++wrong_owner.session;
                    wrong = agent::AuthorizedClient(server, wrong_owner);
                    WriteFile(server, &byte, 1, &count, nullptr);
                    FlushFileBuffers(server);
                }
            }
        });
        HANDLE client = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        check(client != INVALID_HANDLE_VALUE && agent::AuthorizedServer(client, identity),
            "M06 real client validates actual server process token");
        auto wrong_owner = identity; ++wrong_owner.session;
        check(client != INVALID_HANDLE_VALUE && !agent::AuthorizedServer(client, wrong_owner),
            "M06 client rejects mismatched server identity");
        if (client != INVALID_HANDLE_VALUE) {
            char byte = 'x'; DWORD count = 0;
            WriteFile(client, &byte, 1, &count, nullptr);
            ReadFile(client, &byte, 1, &count, nullptr);
            CloseHandle(client);
        } else {
            CancelSynchronousIo(worker.native_handle());
        }
        worker.join();
        check(correct && !wrong, "M06 actual pipe client token is checked before dispatch");
        CloseHandle(server);
    }
    if (restricted) CloseHandle(restricted);
    if (impersonation) CloseHandle(impersonation);
    if (primary) CloseHandle(primary);
    const auto descriptor = [&](const wchar_t* sddl, bool expected, const char* label) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        const bool made = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr) != FALSE;
        check(made && security::PrivateDescriptor(sd, true, true) == expected, label);
        if (sd) LocalFree(sd);
    };
    descriptor(L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", true, "M05 canonical private service descriptor accepted");
    descriptor(L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GR;;;AU)", false, "M05 authenticated-user grant rejected");
    descriptor(L"O:BAG:SYD:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", false, "M05 inheritable parent permissions rejected at root");
    descriptor(L"O:AUG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", false, "M05 untrusted owner cannot control a nominally private DACL");
    descriptor(L"O:BAG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)", false, "M05 directory without private child inheritance rejected");

    using namespace pulse::index;
    const auto root = fs::absolute(fs::path("bench_data") / ("security-p1-" + std::to_string(GetCurrentProcessId())));
    const fs::path artifact = L"v9/0123456789abcdef/base-a.bin";
    auto write = [](const fs::path& path, const char* text) {
        fs::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary) << text;
    };
    const auto source = root / "source", public_target = root / "public", existing = root / "existing", empty = root / "empty";
    write(source / artifact, "isolated-index-metadata");
    write(public_target / "unrelated.txt", "do-not-touch");
    write(existing / artifact, "isolated-index-metadata");
    IndexMigration migration; std::wstring error;
    check(!CopyIndexForMigration(source.wstring(), public_target.wstring(), migration, error, true) &&
        migration.failure == ERROR_ACCESS_DENIED && !fs::exists(public_target / artifact) && fs::exists(source / artifact),
        "M05 non-private nonempty target refused before any index bytes are copied");
    check(fs::exists(public_target / "unrelated.txt") && !security::PrivateObject(public_target.wstring(), true),
        "M05 unrelated destination data and permissions are not silently hardened");
    check(!CopyIndexForMigration(source.wstring(), existing.wstring(), migration, error, true) &&
        migration.failure == ERROR_ACCESS_DENIED && fs::exists(source / artifact),
        "M05 identical preexisting public copy cannot bypass confidentiality validation");
    const bool copied = CopyIndexForMigration(source.wstring(), empty.wstring(), migration, error, true);
    check(copied ? security::PrivateTree(empty.wstring()) && fs::exists(empty / artifact)
                 : migration.failure == ERROR_ACCESS_DENIED && !fs::exists(empty / artifact),
        "M05 empty target either becomes verified private before copy or fails without payload");
    std::printf("[INFO] M05 private migration success under this process token: %s\n", copied ? "yes" : "no (not elevated / owner assignment denied)");
    check(fs::exists(source / artifact), "M05 original source remains until explicit migration commit");
    std::error_code cleanup;
    fs::remove_all(root, cleanup);
    if (cleanup) std::printf("[INFO] protected fixture cleanup requires owner/admin: %lu\n", static_cast<unsigned long>(cleanup.value()));
    return failures ? 1 : 0;
}
