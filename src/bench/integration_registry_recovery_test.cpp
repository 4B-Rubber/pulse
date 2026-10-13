#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <iostream>

namespace fault {
bool enabled = false;
unsigned events = 0, writes = 0, flushes = 0, crash_at = 0, fail_flush = 0;
std::set<unsigned> fail_writes;
void Event(LONG result) {
    if (!enabled || (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)) return;
    if (++events == crash_at) TerminateProcess(GetCurrentProcess(), 77);
}
LSTATUS WINAPI Set(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) {
    if (enabled && fail_writes.contains(++writes)) return ERROR_ACCESS_DENIED;
    const auto result = RegSetValueExW(key, name, reserved, type, data, size);
    Event(result); return result;
}
LSTATUS WINAPI Delete(HKEY key, LPCWSTR name) {
    if (enabled && fail_writes.contains(++writes)) return ERROR_ACCESS_DENIED;
    const auto result = RegDeleteValueW(key, name);
    Event(result); return result;
}
LSTATUS WINAPI Flush(HKEY key) {
    if (enabled && ++flushes == fail_flush) return ERROR_ACCESS_DENIED;
    const auto result = RegFlushKey(key);
    Event(result); return result;
}
void Reset() { enabled = false; events = writes = flushes = crash_at = fail_flush = 0; fail_writes.clear(); }
}

#define RegSetValueExW fault::Set
#define RegDeleteValueW fault::Delete
#define RegFlushKey fault::Flush
#include "../app/shell_integration_registry.cpp"
#undef RegSetValueExW
#undef RegDeleteValueW
#undef RegFlushKey

namespace {
using namespace pulse::app;
const std::wstring old_exe = L"C:\\Private fixture\\pulse.exe", new_exe = L"D:\\Private fixture\\pulse.exe";
const std::wstring shell = L"Software\\Classes\\Directory\\shell";
const std::wstring command = shell + L"\\open\\command";
const std::wstring backup = L"Software\\Pulse\\ShellIntegration\\Backups\\v1\\Directory";
const std::wstring prefix = L"Software\\PulseTest\\IntegrationRecovery-";
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!ok) ++failures;
}
Value At(const std::wstring& key, const std::wstring& name = L"") {
    Value value; if (!Read(key, name, value)) { Check(false, "read private registry fixture"); }
    return value;
}
std::vector<Value> Original() {
    return {At(command), At(command, L"DelegateExecute"), At(shell), At(shell + L"\\open", L"DelegateExecute")};
}
void ResetHive() {
    fault::Reset();
    const auto result = RegDeleteTreeW(HKEY_CURRENT_USER, L"Software");
    Check(result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND, "reset only redirected private Software tree");
}
void SeedOriginal() {
    Value binary; binary.exists = true; binary.type = REG_BINARY; binary.bytes = {0, 7, 255, 0};
    Check(Write(command, L"", binary) && Write(command, L"DelegateExecute", String(L"original delegate")) &&
          Write(shell, L"", String(L"browse")), "seed typed original values and absent verb delegate");
}
void SeedUpgrade() {
    ResetHive(); SeedOriginal();
    Check(ApplyShellIntegration(ShellIntegrationKind::Directory, old_exe, true), "seed old executable ownership");
}
void SeedLegacy() {
    ResetHive();
    for (const auto* group : {L"Directory", L"Drive", L"WinE", L"ThisPc"}) {
        const auto bindings = Bindings(group, L"");
        for (size_t i = 0; i < bindings.size(); ++i) {
            const bool absent = i == 0 || (i == 1 &&
                (group == std::wstring_view(L"Directory") || group == std::wstring_view(L"Drive")));
            if (!absent) Check(Write(bindings[i].key, bindings[i].name, bindings[i].desired), "seed isolated legacy value");
        }
    }
    Check(HasLegacyShellIntegrationResidue(), "complete legacy signature recognized");
}
bool LegacyGone() {
    for (const auto* group : {L"Directory", L"Drive", L"WinE", L"ThisPc"})
        for (const auto& binding : Bindings(group, L""))
            if (At(binding.key, binding.name).exists) return false;
    return true;
}
bool Action(const std::wstring& action) {
    if (action == L"upgrade") return UpgradeShellIntegration(ShellIntegrationKind::Directory, old_exe, new_exe);
    if (action == L"repair") return RepairLegacyShellIntegrationResidue();
    if (action == L"disable-old") return ApplyShellIntegration(ShellIntegrationKind::Directory, old_exe, false);
    if (action == L"disable-new") return ApplyShellIntegration(ShellIntegrationKind::Directory, new_exe, false);
    return false;
}
DWORD Child(const std::wstring& hive, const std::wstring& nonce, const std::wstring& action, unsigned crash) {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, _countof(executable))) return 999;
    std::wstring arguments = L"\"" + std::wstring(executable) + L"\" --child \"" + hive + L"\" " +
        nonce + L" " + action + L" " + std::to_wstring(crash);
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, arguments.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 999;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 998); WaitForSingleObject(process.hProcess, 5000); }
    DWORD code = 999; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
    return code;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 6 && std::wstring_view(argv[1]) == L"--child") {
        const std::wstring hive = argv[2], nonce = argv[3];
        if (!hive.starts_with(prefix) || hive.find(L'\\', prefix.size()) != std::wstring::npos) return 90;
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, hive.c_str(), 0, KEY_ALL_ACCESS, &root) != ERROR_SUCCESS) return 91;
        wchar_t owner[128]{}; DWORD size = sizeof(owner), type = 0;
        const bool owned = RegQueryValueExW(root, L"FixtureOwner", nullptr, &type,
            reinterpret_cast<BYTE*>(owner), &size) == ERROR_SUCCESS && type == REG_SZ &&
            size > 0 && size <= sizeof(owner) && owner[_countof(owner) - 1] == 0 && nonce == owner;
        if (!owned || RegOverridePredefKey(HKEY_CURRENT_USER, root) != ERROR_SUCCESS) { RegCloseKey(root); return 92; }
        fault::enabled = true; fault::crash_at = static_cast<unsigned>(_wtoi(argv[5]));
        const bool ok = Action(argv[4]);
        fault::enabled = false;
        const DWORD events = fault::events;
        RegSetValueExW(root, L"EventCount", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&events), sizeof(events));
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(root);
        return ok ? 0 : 1;
    }
    if (argc != 1) return 2;
    const std::wstring nonce = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::wstring hive = prefix + nonce;
    HKEY root = nullptr; DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, hive.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                        &root, &disposition) != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
        if (root) RegCloseKey(root); return 2;
    }
    if (RegSetValueExW(root, L"FixtureOwner", 0, REG_SZ, reinterpret_cast<const BYTE*>(nonce.c_str()),
                      static_cast<DWORD>((nonce.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS ||
        RegOverridePredefKey(HKEY_CURRENT_USER, root) != ERROR_SUCCESS) { RegCloseKey(root); return 2; }
    std::wcout << L"[FIXTURE] " << hive << std::endl;
    ResetHive(); SeedOriginal(); const auto original = Original();
    for (const auto* action : {L"upgrade", L"repair"}) {
        if (std::wstring_view(action) == L"upgrade") SeedUpgrade(); else SeedLegacy();
        Check(Child(hive, nonce, action, 0) == 0, "uninterrupted child operation succeeds");
        DWORD events = 0, size = sizeof(events), type = 0;
        Check(RegQueryValueExW(root, L"EventCount", nullptr, &type, reinterpret_cast<BYTE*>(&events), &size) == ERROR_SUCCESS &&
              type == REG_DWORD && events > 0 && events < 100, "capture actual production write and flush boundaries");
        for (unsigned point = 1; point <= events && point < 100; ++point) {
            std::cout << "[BOUNDARY] " << (std::wstring_view(action) == L"upgrade" ? "upgrade " : "legacy ") << point << std::endl;
            if (std::wstring_view(action) == L"upgrade") SeedUpgrade(); else SeedLegacy();
            Check(Child(hive, nonce, action, point) == 77, "child terminates at requested durable transaction boundary");
            if (std::wstring_view(action) == L"upgrade") {
                Check(Child(hive, nonce, L"upgrade", 0) == 0 && ReadShellIntegration(ShellIntegrationKind::Directory, new_exe),
                      "restart resumes upgrade after any write or flush boundary");
                Check(Child(hive, nonce, L"disable-new", 0) == 0 && Original() == original,
                      "upgraded restore retains exact binary empty and absent original states");
                for (const auto* disable : {L"disable-old", L"disable-new"}) {
                    SeedUpgrade(); Check(Child(hive, nonce, action, point) == 77, "recreate interrupted upgrade for direct restore");
                    if (At(backup, L"UpgradeJournal").exists)
                        Check(Child(hive, nonce, disable, 0) == 0 && Original() == original,
                              "old and new executable can restore an interrupted upgrade before ownership early return");
                }
            } else {
                const auto result = Child(hive, nonce, L"repair", 0);
                Check((result == 0 || result == 1) && LegacyGone() && !HasLegacyShellIntegrationResidue(),
                      "restart completes legacy deletion or recognizes already completed repair");
                Check(At(L"Software\\Pulse\\ShellIntegration\\Backups\\v1\\LegacyOrphanRepair", L"Snapshot").exists,
                      "legacy original proof remains after recovery");
            }
        }
    }
    for (unsigned failed_write = 2; failed_write <= 5; ++failed_write) {
        for (unsigned failed_rollback = failed_write + 1; failed_rollback <= failed_write + 3; ++failed_rollback) {
            ResetHive(); SeedOriginal(); fault::Reset(); fault::enabled = true;
            fault::fail_writes = {failed_write, failed_rollback};
            Check(!ApplyShellIntegration(ShellIntegrationKind::Directory, old_exe, true), "injected enable failure is reported");
            fault::enabled = false;
            if (failed_write == 4 && failed_rollback == 5)
                Check(HasShellIntegrationOwnership(ShellIntegrationKind::Directory, old_exe), "delegate rollback failure retains owned command anchor");
            Check(ApplyShellIntegration(ShellIntegrationKind::Directory, old_exe, false) && Original() == original,
                  "retry disable repairs write and compensation failure pair exactly");
        }
    }
    SeedUpgrade(); fault::Reset(); fault::enabled = true; fault::fail_flush = 1;
    Check(!UpgradeShellIntegration(ShellIntegrationKind::Directory, old_exe, new_exe), "journal flush failure aborts upgrade");
    fault::enabled = false;
    Check(ReadShellIntegration(ShellIntegrationKind::Directory, old_exe), "failed journal flush changes no association");
    Check(UpgradeShellIntegration(ShellIntegrationKind::Directory, old_exe, new_exe), "retry flushes surviving journal before upgrade");
    SeedUpgrade(); fault::Reset(); fault::enabled = true;
    Check(UpgradeShellIntegration(ShellIntegrationKind::Directory, old_exe, new_exe), "measure successful upgrade flush count");
    const unsigned upgrade_flushes = fault::flushes; fault::enabled = false;
    for (unsigned flush = 1; flush <= upgrade_flushes; ++flush) {
        SeedUpgrade(); fault::Reset(); fault::enabled = true; fault::fail_flush = flush;
        Check(!UpgradeShellIntegration(ShellIntegrationKind::Directory, old_exe, new_exe), "each failed durability confirmation is reported");
        fault::enabled = false;
        Check(UpgradeShellIntegration(ShellIntegrationKind::Directory, old_exe, new_exe) &&
              ApplyShellIntegration(ShellIntegrationKind::Directory, new_exe, false) && Original() == original,
              "flush failure including final journal removal remains recoverable");
    }
    SeedUpgrade(); Check(Child(hive, nonce, L"upgrade", 2) == 77, "prepare interrupted upgrade for restoration boundary sweep");
    Check(Child(hive, nonce, L"disable-new", 0) == 0, "baseline journal restoration succeeds");
    DWORD restore_events = 0, restore_size = sizeof(restore_events);
    Check(RegQueryValueExW(root, L"EventCount", nullptr, nullptr, reinterpret_cast<BYTE*>(&restore_events), &restore_size) == ERROR_SUCCESS &&
          restore_events > 0 && restore_events < 100, "capture restoration commit boundaries");
    for (unsigned point = 1; point <= restore_events && point < 100; ++point) {
        SeedUpgrade(); Check(Child(hive, nonce, L"upgrade", 2) == 77, "recreate pending journal for restoration interruption");
        Check(Child(hive, nonce, L"disable-new", point) == 77, "interrupt journal restoration at write or flush");
        Check(Child(hive, nonce, L"disable-new", 0) == 0 && Original() == original,
              "interrupted restoration converges even after Snapshot deletion");
    }
    SeedLegacy(); fault::Reset(); fault::enabled = true; fault::fail_flush = 1;
    Check(!RepairLegacyShellIntegrationResidue(), "legacy journal flush failure aborts before deletion");
    fault::enabled = false;
    Check(HasLegacyShellIntegrationResidue() && RepairLegacyShellIntegrationResidue() && LegacyGone(),
          "legacy flush failure retains proof and can resume");
    SeedLegacy(); fault::Reset(); fault::enabled = true; fault::fail_writes = {3, 4};
    Check(!RepairLegacyShellIntegrationResidue(), "partial legacy deletion failure is reported without destructive compensation");
    fault::enabled = false;
    Check(HasLegacyShellIntegrationResidue() && RepairLegacyShellIntegrationResidue() && LegacyGone(),
          "legacy deletion failure resumes from journal after signature changed");
    SeedUpgrade(); Check(Child(hive, nonce, L"upgrade", 2) == 77, "leave flushed upgrade journal before association writes");
    const auto foreign = String(L"third party took ownership");
    Check(Write(command, L"", foreign), "inject later third-party command into private hive");
    Check(!UpgradeShellIntegration(ShellIntegrationKind::Directory, old_exe, new_exe) &&
          !ApplyShellIntegration(ShellIntegrationKind::Directory, new_exe, false) && SameValue(At(command), foreign),
          "journal recovery reports conflict and preserves third-party value");
    SeedLegacy(); Check(Child(hive, nonce, L"repair", 3) == 77, "interrupt legacy repair after first deletion");
    Check(HasLegacyShellIntegrationResidue(), "partially deleted legacy signature remains discoverable through journal");
    Check(Write(command, L"", foreign) && !RepairLegacyShellIntegrationResidue() && SameValue(At(command), foreign),
          "legacy recovery preserves later third-party command and reports conflict");
    SeedUpgrade(); const auto owned = Original();
    Check(Write(backup, L"UpgradeJournal", String(L"invalid journal")) &&
          !UpgradeShellIntegration(ShellIntegrationKind::Directory, old_exe, new_exe) &&
          !ApplyShellIntegration(ShellIntegrationKind::Directory, old_exe, false) && Original() == owned,
          "malformed journal fails closed without association writes");
    ResetHive();
    auto expandable = String(L"%PRIVATE_FIXTURE%\\original.exe %1"); expandable.type = REG_EXPAND_SZ;
    Check(Write(command, L"", expandable) && Write(command, L"DelegateExecute", String(L"")),
          "seed expandable command empty delegate and absent shell default");
    const auto typed_original = Original();
    Check(ApplyShellIntegration(ShellIntegrationKind::Directory, old_exe, true) &&
          Child(hive, nonce, L"upgrade", 2) == 77 && Child(hive, nonce, L"disable-old", 0) == 0 &&
          Original() == typed_original, "journal restoration preserves expandable type empty bytes and absent values");
    fault::Reset();
    const bool detached = RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) == ERROR_SUCCESS;
    RegCloseKey(root);
    Check(detached, "detach private predefined-key override");
    if (detached) Check(RegDeleteTreeW(HKEY_CURRENT_USER, hive.c_str()) == ERROR_SUCCESS, "remove exclusively created fixture hive");
    return failures ? 1 : 0;
}
