#include "../app/shell_tag_registry.h"
#include "../app/shell_tag_protocol.h"
#include <filesystem>
#include <iostream>

namespace {
int create_count = 0, set_count = 0, fail_create = 0, fail_set = 0;
bool fail_icon = false, fail_delete = false;
int delete_count = 0, fail_delete_at = 0;
LSTATUS WINAPI Create(HKEY root, LPCWSTR key, DWORD reserved, LPWSTR cls, DWORD options,
    REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY result, LPDWORD disposition) {
    if (++create_count == fail_create) return ERROR_ACCESS_DENIED;
    return RegCreateKeyExW(root, key, reserved, cls, options, access, security, result, disposition);
}
LSTATUS WINAPI Set(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) {
    if (++set_count == fail_set) return ERROR_ACCESS_DENIED;
    return RegSetValueExW(key, name, reserved, type, data, size);
}
LSTATUS WINAPI Remove(HKEY key, LPCWSTR path) {
    if (++delete_count == fail_delete_at) return ERROR_ACCESS_DENIED;
    if (fail_delete && std::wstring(path).find(L"Directory") != std::wstring::npos) return ERROR_ACCESS_DENIED;
    return SHDeleteKeyW(key, path);
}
HANDLE WINAPI TestCreateFile(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD creation, DWORD attributes, HANDLE templ) {
    if (fail_icon) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    return CreateFileW(path, access, share, security, creation, attributes, templ);
}
}
int main() {
    using namespace pulse::app;
    namespace fs = std::filesystem;
    const std::wstring sandbox = L"Software\\PulseTest\\TagRegistry-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HKEY registry_root = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sandbox.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &registry_root, nullptr) != ERROR_SUCCESS) return 2;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, registry_root) != ERROR_SUCCESS) { RegCloseKey(registry_root); return 2; }
    const auto base = fs::absolute(fs::path(L"bench_data")).lexically_normal();
    const auto fixture = base / (L"tag-registry-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(base);
    if (!fs::create_directory(fixture)) {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegCloseKey(registry_root);
        RegDeleteTreeW(HKEY_CURRENT_USER, sandbox.c_str());
        return 2;
    }
    const auto icons = (fixture / L"icons").wstring();
    const std::wstring exe = (fixture / L"pulse.exe").wstring(), title = L"Pulse tags";
    const std::vector<ColorTag> tags{{L"fixture-id", L"fixture tag", 0x123456, {}}};
    const TagMenuRegistryApi api{Create, Set, Remove, TestCreateFile};
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; failures += !ok; };
    check(InstallTagMenu(tags, exe, title, icons, api) && TagMenuMatches(tags, exe, title, icons),
          "complete COM-backed file directory and server registration installs and verifies");
    const int baseline_creates = create_count, baseline_sets = set_count;
    check(baseline_creates == 9 && baseline_sets == 24,
          "fault matrix covers both cascading roots commands tag verbs and COM server");
    for (int operation = 0; operation < 2; ++operation) {
        const int total = operation ? baseline_sets : baseline_creates;
        for (int failure = 1; failure <= total; ++failure) {
            create_count = set_count = 0;
            fail_create = operation ? 0 : failure;
            fail_set = operation ? failure : 0;
            const bool installed = InstallTagMenu(tags, exe, title, icons, api);
            check(!installed, "each parent/verb/command write failure prevents successful installation acknowledgement");
            fail_create = fail_set = 0;
            check(InstallTagMenu(tags, exe, title, icons, api) && TagMenuMatches(tags, exe, title, icons),
                  "unchanged tag list fully recovers after transient write failure");
        }
    }
    const std::vector<ColorTag> updated{{L"fixture-id", L"updated tag", 0x456789, {}},
                                        {L"second-id", L"second tag", 0x654321, {}}};
    const auto updated_exe = (fixture / L"updated pulse.exe").wstring();
    const std::wstring updated_title = L"Updated Pulse tags";
    create_count = set_count = 0;
    check(InstallTagMenu(updated, updated_exe, updated_title, icons, api) &&
          TagMenuMatches(updated, updated_exe, updated_title, icons),
          "existing live COM menu updates its executable title tag names colors and selection verbs");
    const int update_creates = create_count, update_sets = set_count;
    check(update_creates > baseline_creates && update_sets > baseline_sets,
          "update failure matrix includes the additional tag in both roots");
    for (int operation = 0; operation < 2; ++operation) {
        const int total = operation ? update_sets : update_creates;
        for (int failure = 1; failure <= total; ++failure) {
            check(InstallTagMenu(tags, exe, title, icons, api), "update fault starts from complete prior desired menu");
            create_count = set_count = 0;
            fail_create = operation ? 0 : failure;
            fail_set = operation ? failure : 0;
            check(!InstallTagMenu(updated, updated_exe, updated_title, icons, api),
                  "existing-live update write failure never reports synchronized");
            fail_create = fail_set = 0;
            check(InstallTagMenu(updated, updated_exe, updated_title, icons, api) &&
                  TagMenuMatches(updated, updated_exe, updated_title, icons),
                  "same updated desired repairs every failed COM publication write");
        }
    }
    const std::wstring directory_root = L"Software\\Classes\\Directory\\shell\\PulseTags";
    const auto command = directory_root + L"\\shell\\" + shell_tags::VerbName(updated[0].id) + L"\\command";
    check(SHDeleteKeyW(HKEY_CURRENT_USER, command.c_str()) == ERROR_SUCCESS &&
          !TagMenuMatches(updated, updated_exe, updated_title, icons),
          "missing directory DelegateExecute command invalidates complete registration");
    check(InstallTagMenu(updated, updated_exe, updated_title, icons, api) &&
          TagMenuMatches(updated, updated_exe, updated_title, icons), "unchanged desired repairs missing COM command");
    // Registration replaces owned trees, removing obsolete legacy visibility values.
    for (const auto* root : {L"Software\\Classes\\*\\shell\\PulseTags", directory_root.c_str()}) {
        HKEY key = nullptr;
        check(RegOpenKeyExW(HKEY_CURRENT_USER, root, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS,
              "open isolated menu root for obsolete-value fixture");
        if (key) {
            const wchar_t empty = 0;
            for (const auto* name : {L"LegacyDisable", L"", L"Extended"})
                check(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(&empty), sizeof(empty)) == ERROR_SUCCESS,
                      "prepare obsolete legacy visibility value");
            RegCloseKey(key);
        }
    }
    check(InstallTagMenu(updated, updated_exe, updated_title, icons, api),
          "reapplying desired COM registration removes obsolete root values");
    for (const auto* root : {L"Software\\Classes\\*\\shell\\PulseTags", directory_root.c_str()}) {
        HKEY key = nullptr;
        check(RegOpenKeyExW(HKEY_CURRENT_USER, root, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS,
              "open repaired isolated root");
        if (key) {
            for (const auto* name : {L"LegacyDisable", L"", L"Extended"})
                check(RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND,
                      "obsolete legacy root value is absent after repair");
            RegCloseKey(key);
        }
    }
    for (int failure = 1; failure <= 3; ++failure) {
        check(InstallTagMenu(tags, exe, title, icons, api), "prepare file directory and COM server removal fixture");
        delete_count = 0; fail_delete_at = failure;
        check(!RemoveTagMenu(icons, api), "each file directory or COM server deletion failure remains retryable");
        fail_delete_at = 0;
        check(RemoveTagMenu(icons, api), "unchanged disable repairs every failed registration-root deletion");
    }
    fail_icon = true;
    check(!InstallTagMenu(tags, exe, title, icons, api), "failed icon creation prevents success acknowledgement");
    fail_icon = false;
    check(InstallTagMenu(tags, exe, title, icons, api), "unchanged tag list retries missing icon successfully");
    fail_delete = true;
    check(!RemoveTagMenu(icons, api), "directory-root removal failure is reported after file root was removed");
    fail_delete = false;
    check(RemoveTagMenu(icons, api) && !TagMenuMatches(tags, exe, title, icons), "disable retries and removes a directory-only residual menu");
    check(RemoveTagMenu(icons, api), "repeated complete removal is idempotent");
    check(RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) == ERROR_SUCCESS, "isolated HKCU override removed");
    RegCloseKey(registry_root);
    check(RegDeleteTreeW(HKEY_CURRENT_USER, sandbox.c_str()) == ERROR_SUCCESS, "isolated registry fixture removed");
    std::error_code cleanup_error;
    if (fixture.parent_path() == base) fs::remove_all(fixture, cleanup_error);
    check(!cleanup_error && !fs::exists(fixture), "isolated icon fixture removed");
    return failures ? 1 : 0;
}
