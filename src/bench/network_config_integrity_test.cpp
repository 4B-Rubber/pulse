#include <windows.h>
#include <winnetwk.h>
#include "../index/network_index.h"
#include "../index/index_config.h"
#include "../common/config_json.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
std::wstring fixture_directory;
std::atomic<unsigned> blocked_network_io{0};
bool NetworkPath(LPCWSTR path) {
    const std::wstring_view value(path ? path : L"");
    return value.starts_with(L"\\\\?\\UNC\\") ||
        (value.starts_with(L"\\\\") && !value.starts_with(L"\\\\?\\"));
}
HANDLE WINAPI ConfigCreateFile(LPCWSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
                               DWORD disposition, DWORD attributes, HANDLE model) {
    if (NetworkPath(path)) {
        ++blocked_network_io;
        SetLastError(ERROR_BAD_NETPATH);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(path, access, sharing, security, disposition, attributes, model);
}
HANDLE WINAPI ConfigFindFirst(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
                              FINDEX_SEARCH_OPS operation, LPVOID filter, DWORD flags) {
    if (NetworkPath(path)) {
        ++blocked_network_io;
        SetLastError(ERROR_BAD_NETPATH);
        return INVALID_HANDLE_VALUE;
    }
    return FindFirstFileExW(path, level, data, operation, filter, flags);
}
}
namespace pulse::index { std::wstring ConfigTestUserIndexRoot() { return fixture_directory; } }
#define UserIndexRoot ConfigTestUserIndexRoot
#define CreateFileW ConfigCreateFile
#define FindFirstFileExW ConfigFindFirst
#include "../index/network_index.cpp"
#undef FindFirstFileExW
#undef CreateFileW
#undef UserIndexRoot

namespace {
void Write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}
int main() {
    using namespace pulse::index;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        failures += !ok;
    };
    const auto temp = std::filesystem::temp_directory_path();
    const auto fixture = temp / (L"pulse-network-config-test-" + std::to_wstring(GetCurrentProcessId()) +
                                L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    if (!std::filesystem::create_directory(fixture, ec)) return 1;
    fixture_directory = fixture.wstring();
    const auto config = fixture / L"network-index.json";
    std::vector<std::wstring> roots{L"sentinel"};
    std::wstring error;
    check(LoadNetworkRootsFile(config.wstring(), roots, &error) && roots.empty(),
          "missing configuration is an empty initial setup");
    Write(config, R"({"version":1,"roots":[]})");
    check(LoadNetworkRootsFile(config.wstring(), roots, &error) && roots.empty(),
          "explicit empty roots array is valid");
    Write(config, R"({"nested":{"roots":["ignored"]},"roots":[]})");
    check(LoadNetworkRootsFile(config.wstring(), roots, &error) && roots.empty(),
          "only the top-level roots member is used");
    const std::vector<std::string> damaged = {
        R"({"roots":["\\\\server\\share"])",
        R"({"roots":true})",
        R"({"roots":[9]})",
        R"({"roots":[],"roots":[]})",
        R"({"roots":[],"\u0072oots":[]})",
        R"({"roots":[]} trailing)",
        R"({"roots":[],})",
        R"({"roots":["\q"]})",
        R"({"roots":["\\\\server\\share\\\uD800"]})",
        R"({"other":[]})",
        std::string("{\"roots\":[\"") + "\xC0\xAF" + "\"]}"
    };
    for (const auto& bytes : damaged) {
        Write(config, bytes);
        roots = {L"sentinel"};
        error.clear();
        check(!LoadNetworkRootsFile(config.wstring(), roots, &error) && roots == std::vector<std::wstring>{L"sentinel"} &&
              !error.empty(), "damaged/schema-invalid config fails without modifying caller state");
        NetworkIndex index;
        index.Start(nullptr, 0, 0);
        index.Stop();
        check(!index.ConfigError().empty() &&
              !index.AddRoot(L"\\\\server\\new", &error) &&
              !index.RemoveRoot(L"\\\\server\\share", &error) && Read(config) == bytes,
              "startup config error blocks add/remove and preserves original bytes");
    }
    Write(config, R"({"\u0072oots":["\\\\server\\share\\\u4E2D\uD83D\uDE00"]})");
    check(LoadNetworkRootsFile(config.wstring(), roots, &error) && roots.size() == 1 &&
          roots.front() == L"\\\\server\\share\\中\xD83D\xDE00",
          "Unicode key, path escapes and surrogate pairs decode correctly");
    std::vector<std::wstring> escaped;
    check(pulse::json::ConfigSyntax(LR"({"roots":["\"\\\/\b\f\n\r\t"]})", false).StringArrayMember(L"roots", escaped) &&
          escaped == std::vector<std::wstring>{L"\"\\/\b\f\n\r\t"},
          "strict reader decodes every standard one-character JSON escape");
    Write(config, R"({"roots":[]})");
    const auto before = Read(config);
    HANDLE locked = CreateFileW(config.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "isolated fixture denies read while allowing replacement");
    if (locked != INVALID_HANDLE_VALUE) {
        roots = {L"sentinel"};
        check(!LoadNetworkRootsFile(config.wstring(), roots, &error) && roots.front() == L"sentinel",
              "read failure preserves caller state");
        NetworkIndex index;
        index.Start(nullptr, 0, 0);
        index.Stop();
        check(!index.ConfigError().empty() && !index.AddRoot(L"\\\\server\\new", &error) &&
              !index.RemoveRoot(L"\\\\server\\share", &error),
              "read failure blocks configuration replacement");
        CloseHandle(locked);
        check(Read(config) == before, "read-failed configuration bytes remain untouched");
        Write(config, R"({"roots":["\\\\server\\existing"]})");
        check(index.AddRoot(L"\\\\server\\new", &error) && index.ConfigError().empty() &&
              LoadNetworkRootsFile(config.wstring(), roots, &error) && roots.size() == 2,
              "repaired file reloads before add and retains its existing roots");
        check(index.RemoveRoot(L"\\\\server\\existing", &error) &&
              LoadNetworkRootsFile(config.wstring(), roots, &error) && roots.size() == 1 && roots.front() == L"\\\\server\\new",
              "remove succeeds after config recovery");
    }
    check(pulse::json::ValidConfigObject(L"{\"color\":0xAABBCC}", true) &&
          !pulse::json::ValidConfigObject(L"{\"color\":0xAABBCC}") &&
          pulse::json::ValidConfigObject(L"{\"unknown\":\"\\uD800\"}"),
          "existing syntax-only validation and legacy hex behavior remain unchanged");
    check(blocked_network_io == 0, "configuration failure never starts network I/O; guard blocks regressions");
    std::error_code guard_error;
    const bool parent_matches = std::filesystem::equivalent(fixture.parent_path(), temp, guard_error);
    const auto status = std::filesystem::symlink_status(fixture, ec);
    const bool owned = parent_matches && !guard_error && !ec &&
        status.type() == std::filesystem::file_type::directory &&
        fixture.filename().wstring().starts_with(L"pulse-network-config-test-");
    if (owned) std::filesystem::remove_all(fixture, ec);
    check(owned && !ec && !std::filesystem::exists(fixture), "private config fixture removed");
    return failures ? 1 : 0;
}
