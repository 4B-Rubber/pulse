// app_prefs.cpp — Persist general settings; sync 开机自启 with the Run key.
#include "app_prefs.h"
#include "session.h"
#include "../ui/panel_metrics.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace pulse::app {
namespace {

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"Pulse";

// A self-test run may point every registry path below into a sandbox key, so the
// reconciliation can be exercised without touching the machine's real settings.
// Production, and any run that does not ask for it, gets the real paths.
std::wstring RegistrySandboxPrefix() {
#ifdef PULSE_WITH_SELFTEST
    wchar_t base[512]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_REGISTRY_BASE", base, ARRAYSIZE(base)) > 0)
        return base;
#endif
    return {};
}

std::wstring RegPath(const std::wstring& path) {
    const std::wstring prefix = RegistrySandboxPrefix();
    return prefix.empty() ? path : prefix + L"\\" + path;
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return n ? std::wstring(path, n) : L"";
}

// The exact Run value this executable installs; one place so the comparison in
// ReconcileRegistryWithFile() cannot drift from what ApplyLaunchOnStartup writes.
std::wstring LaunchOnStartupCommand() {
    const std::wstring exe = ExePath();
    return exe.empty() ? std::wstring() : L"\"" + exe + L"\"";
}

} // namespace

namespace {

void DeleteStoredWallpapers(const std::wstring& dir) {
    static constexpr const wchar_t* kExt[] = {
        L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".jfif", L".img"
    };
    for (const wchar_t* ext : kExt)
        DeleteFileW((dir + L"\\wallpaper" + ext).c_str());
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

} // namespace

namespace {

// Every key ToJson() writes (except "version"), in the order it writes them. A
// file truncated mid-write still parses into a handful of keys, so the count
// doubles as the completeness test that sends Load() to the backup.
constexpr const wchar_t* kStoredKeys[] = {
    L"launch_on_startup", L"keep_running_on_close", L"open_folders_in_pulse",
    L"verify_copies", L"show_status_performance", L"show_pinned_tab_names",
    L"show_hidden_files", L"show_protected_os_files", L"search_pinyin",
    L"global_search_enabled", L"global_search_modifiers", L"global_search_key",
    L"blank_click_go_back", L"change_tracking_enabled", L"change_tracking_days",
    L"theme_mode", L"language", L"window_effect", L"background_image",
    L"row_height", L"sidebar_width", L"address_search_current",
    L"address_search_content", L"tray_icon_size", L"accent_rgb",
    L"custom_tag_colors", L"duplicate_scan_scope", L"duplicate_scan_folder",
    L"duplicate_scan_drive"
};

// Half the keys, rounded up: fewer than this and the file is treated as damaged
// rather than as the user's configuration.
constexpr int kMinStoredKeys = (static_cast<int>(ARRAYSIZE(kStoredKeys)) + 1) / 2;

int CountStoredKeys(const std::wstring& json) {
    int found = 0;
    for (const wchar_t* key : kStoredKeys) {
        if (json.find(L"\"" + std::wstring(key) + L"\"") != std::wstring::npos) ++found;
    }
    return found;
}

} // namespace

void AppPrefs::ResetToDefaults() {
    launch_on_startup = false;
    keep_running_on_close = false;
    open_folders_in_pulse = false;
    verify_copies = false;
    show_status_performance = false;
    show_pinned_tab_names = true;
    search_pinyin = true;
    global_search_enabled = false;
    global_search_modifiers = 1;
    global_search_key = 32;
    show_hidden_files = false;
    show_protected_os_files = false;
    blank_click_go_back = false;
    change_tracking_enabled = false;
    change_tracking_days = 7;
    theme_mode = -1;
    language = L"system";
    window_effect = L"mica-alt";
    background_image.clear();
    row_height = 34;
    sidebar_width = 224;
    address_search_current = false;
    address_search_content = false;
    tray_icon_size = 48;
    accent_rgb.clear();
    custom_tag_colors.clear();
    duplicate_scan_scope = 0;
    duplicate_scan_folder.clear();
    duplicate_scan_drive.clear();
}

std::wstring AppPrefs::ToJson() const {
    std::wstring escaped_effect;
    std::wstring escaped_image;
    std::wstring escaped_language;
    pulse::json::Escape(window_effect, escaped_effect);
    pulse::json::Escape(background_image, escaped_image);
    pulse::json::Escape(language, escaped_language);
    std::wstring out = L"{\n  \"version\":4,\n  \"launch_on_startup\":";
    out += launch_on_startup ? L"true" : L"false";
    out += L",\n  \"keep_running_on_close\":";
    out += keep_running_on_close ? L"true" : L"false";
    out += L",\n  \"open_folders_in_pulse\":";
    out += open_folders_in_pulse ? L"true" : L"false";
    out += L",\n  \"verify_copies\":";
    out += verify_copies ? L"true" : L"false";
    out += L",\n  \"show_status_performance\":";
    out += show_status_performance ? L"true" : L"false";
    out += L",\n  \"show_pinned_tab_names\":";
    out += show_pinned_tab_names ? L"true" : L"false";
    out += L",\n  \"show_hidden_files\":";
    out += show_hidden_files ? L"true" : L"false";
    out += L",\n  \"show_protected_os_files\":";
    out += show_protected_os_files ? L"true" : L"false";
    out += L",\n  \"search_pinyin\":";
    out += search_pinyin ? L"true" : L"false";
    out += L",\n  \"global_search_enabled\":";
    out += global_search_enabled ? L"true" : L"false";
    out += L",\n  \"global_search_modifiers\":" + std::to_wstring(global_search_modifiers);
    out += L",\n  \"global_search_key\":" + std::to_wstring(global_search_key);
    out += L",\n  \"blank_click_go_back\":";
    out += blank_click_go_back ? L"true" : L"false";
    out += L",\n  \"change_tracking_enabled\":";
    out += change_tracking_enabled ? L"true" : L"false";
    out += L",\n  \"change_tracking_days\":";
    out += std::to_wstring(change_tracking_days == 1 || change_tracking_days == 3 ? change_tracking_days : 7);
    out += L",\n  \"theme_mode\":";
    out += std::to_wstring(theme_mode);
    out += L",\n  \"language\":\"";
    out += escaped_language;
    out += L"\"";
    out += L",\n  \"window_effect\":\"";
    out += escaped_effect;
    out += L"\",\n  \"background_image\":\"";
    out += escaped_image;
    out += L"\",\n  \"row_height\":";
    out += std::to_wstring(row_height);
    out += L",\n  \"sidebar_width\":" + std::to_wstring(sidebar_width);
    out += L",\n  \"address_search_current\":" + std::to_wstring(address_search_current);
    out += L",\n  \"address_search_content\":" + std::to_wstring(address_search_content);
    out += L",\n  \"tray_icon_size\":";
    out += std::to_wstring(tray_icon_size);
    out += L",\n  \"accent_rgb\":\"";
    {
        std::wstring escaped_accent;
        pulse::json::Escape(accent_rgb, escaped_accent);
        out += escaped_accent;
    }
    out += L"\",\n  \"custom_tag_colors\":[";
    for (size_t i = 0; i < custom_tag_colors.size(); ++i) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", custom_tag_colors[i] & 0x00FFFFFFu);
        if (i > 0) out += L",";
        out += L"\"";
        out += hex;
        out += L"\"";
    }
    out += L"],\n  \"duplicate_scan_scope\":";
    out += std::to_wstring(duplicate_scan_scope);
    out += L",\n  \"duplicate_scan_folder\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(duplicate_scan_folder, escaped);
        out += escaped;
    }
    out += L"\",\n  \"duplicate_scan_drive\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(duplicate_scan_drive, escaped);
        out += escaped;
    }
    out += L"\"\n}\n";
    return out;
}

bool AppPrefs::FromJson(const std::wstring& json) {
    if (json.empty()) return false;
    launch_on_startup = pulse::json::ExtractBool(json, L"launch_on_startup", false);
    keep_running_on_close = pulse::json::ExtractBool(json, L"keep_running_on_close", false);
    open_folders_in_pulse = pulse::json::ExtractBool(json, L"open_folders_in_pulse", false);
    verify_copies = pulse::json::ExtractBool(json, L"verify_copies", false);
    show_status_performance = pulse::json::ExtractBool(json, L"show_status_performance", false);
    show_pinned_tab_names = pulse::json::ExtractBool(json, L"show_pinned_tab_names", true);
    search_pinyin = pulse::json::ExtractBool(json, L"search_pinyin", true);
    global_search_enabled = pulse::json::ExtractBool(json, L"global_search_enabled", false);
    const int modifiers = pulse::json::ExtractInt(json, L"global_search_modifiers", 1);
    const int key = pulse::json::ExtractInt(json, L"global_search_key", 32);
    global_search_modifiers = modifiers > 0 && modifiers <= 15 ? static_cast<uint32_t>(modifiers) : 1;
    global_search_key = key > 0 && key <= 254 ? static_cast<uint32_t>(key) : 32;
    show_hidden_files = pulse::json::ExtractBool(json, L"show_hidden_files", false);
    show_protected_os_files = pulse::json::ExtractBool(json, L"show_protected_os_files", false);
    blank_click_go_back = pulse::json::ExtractBool(json, L"blank_click_go_back", false);
    change_tracking_enabled = pulse::json::ExtractBool(json, L"change_tracking_enabled", false);
    change_tracking_days = pulse::json::ExtractInt(json, L"change_tracking_days", 7);
    if (change_tracking_days != 1 && change_tracking_days != 3 && change_tracking_days != 7)
        change_tracking_days = 7;
    theme_mode = pulse::json::ExtractInt(json, L"theme_mode", -1);
    if (theme_mode < -1 || theme_mode > 2) theme_mode = -1;
    language = pulse::json::ExtractString(json, L"language", L"system");
    if (language != L"system" && language != L"zh-CN" && language != L"en-US")
        language = L"system";
    window_effect = pulse::json::ExtractString(json, L"window_effect", L"mica-alt");
    if (window_effect == L"dwm-blur") window_effect = L"acrylic-material";
    else if (window_effect.empty()) window_effect = L"mica-alt";
    background_image = pulse::json::ExtractString(json, L"background_image");
    row_height = pulse::json::ExtractInt(json, L"row_height", 34);
    sidebar_width = pulse::json::ExtractInt(json, L"sidebar_width", 224);
    // The stored value is the user's intent; the window caps it while drawing.
    if (sidebar_width < static_cast<int>(ui::kSidebarMinWidthDip) ||
        sidebar_width > static_cast<int>(ui::kPanelWidthMaxDip)) sidebar_width = 224;
    address_search_current = pulse::json::ExtractInt(json, L"address_search_current", 0) != 0;
    address_search_content = pulse::json::ExtractInt(json, L"address_search_content", 0) != 0;
    if (row_height < 24 || row_height > 48) row_height = 34;
    tray_icon_size = pulse::json::ExtractInt(json, L"tray_icon_size", 48);
    if (tray_icon_size < 32 || tray_icon_size > 64) tray_icon_size = 48;
    accent_rgb = pulse::json::ExtractString(json, L"accent_rgb");
    uint32_t accent_parsed = 0;
    if (!accent_rgb.empty() && ParseAccentRgb(accent_rgb, accent_parsed)) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", accent_parsed);
        accent_rgb = hex;
    } else {
        accent_rgb.clear();
    }
    custom_tag_colors.clear();
    for (const std::wstring& entry :
         pulse::json::ExtractStringArray(json, L"custom_tag_colors")) {
        const wchar_t* text = entry.c_str();
        if (*text == L'#') ++text;
        wchar_t* end = nullptr;
        const unsigned long v = wcstoul(text, &end, 16);
        if (end && *end == L'\0' && v <= 0xFFFFFFul && wcslen(text) == 6)
            custom_tag_colors.push_back(static_cast<uint32_t>(v));
    }
    duplicate_scan_scope = pulse::json::ExtractInt(json, L"duplicate_scan_scope", 0);
    if (duplicate_scan_scope < 0 || duplicate_scan_scope > 2) duplicate_scan_scope = 0;
    duplicate_scan_folder = pulse::json::ExtractString(json, L"duplicate_scan_folder");
    duplicate_scan_drive = pulse::json::ExtractString(json, L"duplicate_scan_drive");
    return true;
}

bool AppPrefs::StoreBackgroundImage(const std::wstring& source_path) {
    if (source_path.empty()) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        background_image = source_path;
        return true;
    }
    const wchar_t* ext = PathFindExtensionW(source_path.c_str());
    std::wstring dest = dir + L"\\wallpaper";
    dest += (ext && ext[0]) ? ext : L".img";
    if (!SamePath(source_path, dest)) {
        DeleteStoredWallpapers(dir);
        if (!CopyFileW(source_path.c_str(), dest.c_str(), FALSE)) {
            background_image = source_path;
            return true;
        }
    }
    background_image = dest;
    return true;
}

void AppPrefs::ClearBackgroundImage() {
    const std::wstring dir = GetPulseDataDir();
    if (!dir.empty() && !background_image.empty()) {
        const std::wstring prefix = dir + L"\\wallpaper";
        if (background_image.size() >= prefix.size() &&
            CompareStringOrdinal(background_image.c_str(), static_cast<int>(prefix.size()),
                                 prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL) {
            DeleteFileW(background_image.c_str());
        }
    }
    background_image.clear();
}

bool AppPrefs::ReadLaunchOnStartup() const {
    HKEY key = nullptr;
    const std::wstring run_key = RegPath(kRunKey);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, run_key.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t value[MAX_PATH] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    return value[0] != 0;
}

bool AppPrefs::ApplyLaunchOnStartup(bool on) {
    launch_on_startup = on;
    if (!persist) return true;
    HKEY key = nullptr;
    const std::wstring run_key = RegPath(kRunKey);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, run_key.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG st = ERROR_SUCCESS;
    if (on) {
        const std::wstring cmd = LaunchOnStartupCommand();
        if (cmd.empty()) { RegCloseKey(key); return false; }
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

std::wstring FolderOpenCommandLine(const std::wstring& exe) {
    if (exe.empty()) return {};
    return L"\"" + exe + L"\" \"%1\"";
}

namespace {

// Executable token of a shell command line ("C:\dir with space\pulse.exe" "%1").
std::wstring CommandToken(const std::wstring& command) {
    size_t i = 0;
    while (i < command.size() && iswspace(command[i])) ++i;
    if (i < command.size() && command[i] == L'"') {
        ++i;
        const size_t start = i;
        while (i < command.size() && command[i] != L'"') ++i;
        return command.substr(start, i - start);
    }
    const size_t start = i;
    while (i < command.size() && !iswspace(command[i])) ++i;
    return command.substr(start, i - start);
}

} // namespace

bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe) {
    if (command.empty() || exe.empty()) return false;
    const std::wstring token = CommandToken(command);
    return !token.empty() &&
           CompareStringOrdinal(token.c_str(), -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

namespace {

constexpr const wchar_t* kFolderOpenClasses[] = { L"Directory", L"Drive" };

// "Ours" in the loose sense: the first token names pulse.exe, wherever that copy
// lives. A value left behind by an install that moved (or by an older version) is
// still ours and gets repointed; a verb another program owns is left alone.
bool CommandIsPulse(const std::wstring& command) {
    const std::wstring token = CommandToken(command);
    if (token.empty()) return false;
    const size_t separator = token.find_last_of(L"\\/");
    const std::wstring name = separator == std::wstring::npos
        ? token : token.substr(separator + 1);
    return _wcsicmp(name.c_str(), L"pulse.exe") == 0;
}

std::wstring FolderOpenKey(const wchar_t* cls) {
    return RegPath(std::wstring(L"Software\\Classes\\") + cls + L"\\shell\\open");
}

std::wstring FolderShellKey(const wchar_t* cls) {
    return RegPath(std::wstring(L"Software\\Classes\\") + cls + L"\\shell");
}

// Reads one string value; |name| null means the key's default value.
std::wstring ReadRegString(const std::wstring& key, const wchar_t* name = nullptr) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return {};
    wchar_t value[1024] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(h, name, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(h);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return value;
}

bool WriteFolderOpenClass(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring open = FolderOpenKey(cls);
    const std::wstring command = open + L"\\command";
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, command.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring line = FolderOpenCommandLine(exe);
    const LONG st = RegSetValueExW(h, nullptr, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(line.c_str()),
                                   static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    if (st != ERROR_SUCCESS) return false;
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, open.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t empty[] = L"";
    const LONG de = RegSetValueExW(h, L"DelegateExecute", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
    RegCloseKey(h);
    if (de != ERROR_SUCCESS) return false;

    // HKLM Directory/Drive shell default is "none", so double-click never uses
    // the open verb and falls through to Folder → Explorer. Point HKCU at open.
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, FolderShellKey(cls).c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t open_verb[] = L"open";
    const LONG def = RegSetValueExW(h, nullptr, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(open_verb),
                                    sizeof(open_verb));
    RegCloseKey(h);
    return def == ERROR_SUCCESS;
}

// Ownership is judged loosely here (any command naming a pulse.exe, whatever the
// path), unlike FolderOpenClassIsConfigured: a value left by an install that moved
// still hijacks double-click with an executable that is not there any more, and a
// user who turns the setting off has to be able to turn it off. A command another
// program owns is refused, which is the only line this must not cross.
bool ClearFolderOpenClass(const wchar_t* cls) {
    const std::wstring command = ReadRegString(FolderOpenKey(cls) + L"\\command");
    if (!command.empty() && !CommandIsPulse(command)) return true;
    SHDeleteKeyW(HKEY_CURRENT_USER, FolderOpenKey(cls).c_str());
    const std::wstring shell = FolderShellKey(cls);
    if (_wcsicmp(ReadRegString(shell).c_str(), L"open") == 0) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, shell.c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, nullptr);
            RegCloseKey(h);
        }
    }
    return true;
}

void NotifyAssocChanged() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool FolderOpenClassIsConfigured(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring command = ReadRegString(FolderOpenKey(cls) + L"\\command");
    if (!FolderOpenCommandIsOurs(command, exe)) return false;
    // Keep the repair path for older installs that wrote the command but left
    // the class default verb as "none" (the HKLM default).
    return _wcsicmp(ReadRegString(FolderShellKey(cls)).c_str(), L"open") == 0;
}

bool FolderOpenClassNeedsClear(const wchar_t* cls) {
    const std::wstring command = ReadRegString(FolderOpenKey(cls) + L"\\command");
    // Loose, like the cleanup itself: residue from an install that moved is ours and
    // has to go, otherwise "off" never clears it and it keeps pointing at a path that
    // no longer exists.
    if (!command.empty()) return CommandIsPulse(command);
    // A previous cleanup or an interrupted registration can leave only the
    // HKCU shell default behind; ClearFolderOpenClass removes that residue.
    return _wcsicmp(ReadRegString(FolderShellKey(cls)).c_str(), L"open") == 0;
}

} // namespace

bool AppPrefs::ReadFolderOpen() const {
    const std::wstring exe = ExePath();
    if (exe.empty()) return false;
    const std::wstring command =
        ReadRegString(FolderOpenKey(L"Directory") + L"\\command");
    return FolderOpenCommandIsOurs(command, exe);
}

bool AppPrefs::ApplyFolderOpen(bool on) {
    open_folders_in_pulse = on;
    if (!persist) return true;
    const std::wstring exe = ExePath();
    if (exe.empty()) return false;
    bool ok = true;
    bool changed = false;
    for (const wchar_t* cls : kFolderOpenClasses) {
        if (on) {
            // The only caller is the settings toggle (settings_controller.cpp:373);
            // startup repair belongs to ReconcileRegistryWithFile(). An association
            // that already points at this executable is left alone, so flipping the
            // setting never rebroadcasts a global Explorer refresh.
            if (!FolderOpenClassIsConfigured(cls, exe)) {
                changed = true;
                ok = WriteFolderOpenClass(cls, exe) && ok;
            }
        } else {
            if (FolderOpenClassNeedsClear(cls)) {
                changed = true;
                ok = ClearFolderOpenClass(cls) && ok;
            }
        }
    }
    if (changed) NotifyAssocChanged();
    return ok;
}

bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept {
    const wchar_t* p = text.c_str();
    if (!p || !*p) return false;
    if (*p == L'#') ++p;
    if (wcslen(p) != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (!iswxdigit(p[i])) return false;
    }
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(p, &end, 16);
    if (!end || *end != L'\0' || v > 0xFFFFFFul) return false;
    rgb = static_cast<uint32_t>(v);
    return true;
}

// The main file first, its backup when the main one is missing, unreadable, or
// too incomplete to trust. A backed-up file also heals a truncated one: the next
// Save() writes the merged state back over the damaged main file.
bool AppPrefs::ReadDiskState(AppPrefsValues& values, bool& main_exists,
                             bool* used_backup) const {
    main_exists = false;
    if (used_backup) *used_backup = false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    const std::wstring main_path = dir + L"\\app.json";
    const std::wstring backup_path = main_path + L".bak";
    main_exists = GetFileAttributesW(main_path.c_str()) != INVALID_FILE_ATTRIBUTES;

    std::wstring main_json;
    std::wstring backup_json;
    const bool main_read = ReadUtf8File(main_path, main_json) && !main_json.empty();
    const bool backup_read = ReadUtf8File(backup_path, backup_json) && !backup_json.empty();
    const int main_keys = main_read ? CountStoredKeys(main_json) : 0;
    const int backup_keys = backup_read ? CountStoredKeys(backup_json) : 0;

    // A complete main file wins outright; anything else is compared key by key, so a
    // half-written file never hides the copy that still has the settings in it.
    const std::wstring* source = nullptr;
    if (main_read && main_keys >= kMinStoredKeys) source = &main_json;
    else if (backup_read && backup_keys >= kMinStoredKeys) source = &backup_json;
    else if (main_read && backup_read)
        source = main_keys >= backup_keys ? &main_json : &backup_json;
    else if (main_read) source = &main_json;
    else if (backup_read) source = &backup_json;
    if (!source) return false;
    if (used_backup) *used_backup = source == &backup_json;

    AppPrefs parsed;
    parsed.persist = false;
    if (!parsed.FromJson(*source)) return false;
    values = parsed;
    return true;
}

// A field this process left alone since the last Load()/Save() takes the value the
// file holds now; a field it changed keeps the local value. Notes:
//   - "left alone" is measured against disk_state_, not against the file: a field
//     the disk changed under us must not be mistaken for a local change.
//   - a field missing from this list keeps the local value, so forgetting one is
//     never a lost setting and never a new failure mode.
AppPrefsValues AppPrefs::MergedWithDisk(const AppPrefsValues& disk) const {
    const AppPrefsValues& mine = *this;
    const AppPrefsValues& baseline = disk_state_;
    AppPrefsValues merged = mine;
    if (mine.launch_on_startup == baseline.launch_on_startup)
        merged.launch_on_startup = disk.launch_on_startup;
    if (mine.keep_running_on_close == baseline.keep_running_on_close)
        merged.keep_running_on_close = disk.keep_running_on_close;
    if (mine.open_folders_in_pulse == baseline.open_folders_in_pulse)
        merged.open_folders_in_pulse = disk.open_folders_in_pulse;
    if (mine.verify_copies == baseline.verify_copies)
        merged.verify_copies = disk.verify_copies;
    if (mine.show_status_performance == baseline.show_status_performance)
        merged.show_status_performance = disk.show_status_performance;
    if (mine.show_pinned_tab_names == baseline.show_pinned_tab_names)
        merged.show_pinned_tab_names = disk.show_pinned_tab_names;
    if (mine.search_pinyin == baseline.search_pinyin)
        merged.search_pinyin = disk.search_pinyin;
    if (mine.global_search_enabled == baseline.global_search_enabled)
        merged.global_search_enabled = disk.global_search_enabled;
    if (mine.global_search_modifiers == baseline.global_search_modifiers)
        merged.global_search_modifiers = disk.global_search_modifiers;
    if (mine.global_search_key == baseline.global_search_key)
        merged.global_search_key = disk.global_search_key;
    if (mine.show_hidden_files == baseline.show_hidden_files)
        merged.show_hidden_files = disk.show_hidden_files;
    if (mine.show_protected_os_files == baseline.show_protected_os_files)
        merged.show_protected_os_files = disk.show_protected_os_files;
    if (mine.blank_click_go_back == baseline.blank_click_go_back)
        merged.blank_click_go_back = disk.blank_click_go_back;
    if (mine.change_tracking_enabled == baseline.change_tracking_enabled)
        merged.change_tracking_enabled = disk.change_tracking_enabled;
    if (mine.change_tracking_days == baseline.change_tracking_days)
        merged.change_tracking_days = disk.change_tracking_days;
    if (mine.theme_mode == baseline.theme_mode)
        merged.theme_mode = disk.theme_mode;
    if (mine.language == baseline.language)
        merged.language = disk.language;
    if (mine.window_effect == baseline.window_effect)
        merged.window_effect = disk.window_effect;
    if (mine.background_image == baseline.background_image)
        merged.background_image = disk.background_image;
    if (mine.row_height == baseline.row_height)
        merged.row_height = disk.row_height;
    if (mine.sidebar_width == baseline.sidebar_width)
        merged.sidebar_width = disk.sidebar_width;
    if (mine.address_search_current == baseline.address_search_current)
        merged.address_search_current = disk.address_search_current;
    if (mine.address_search_content == baseline.address_search_content)
        merged.address_search_content = disk.address_search_content;
    if (mine.tray_icon_size == baseline.tray_icon_size)
        merged.tray_icon_size = disk.tray_icon_size;
    if (mine.accent_rgb == baseline.accent_rgb)
        merged.accent_rgb = disk.accent_rgb;
    if (mine.custom_tag_colors == baseline.custom_tag_colors)
        merged.custom_tag_colors = disk.custom_tag_colors;
    if (mine.duplicate_scan_scope == baseline.duplicate_scan_scope)
        merged.duplicate_scan_scope = disk.duplicate_scan_scope;
    if (mine.duplicate_scan_folder == baseline.duplicate_scan_folder)
        merged.duplicate_scan_folder = disk.duplicate_scan_folder;
    if (mine.duplicate_scan_drive == baseline.duplicate_scan_drive)
        merged.duplicate_scan_drive = disk.duplicate_scan_drive;
    return merged;
}

namespace {

// The data directory was redirected, which only a self-test does: that run must not
// touch the machine's registry (see Load()). A registry sandbox lifts that ban for
// the copies it redirects, because those live under a key the test owns.
bool DataDirRedirected() {
#ifdef PULSE_WITH_SELFTEST
    wchar_t probe[2]{};
    return GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", probe, ARRAYSIZE(probe)) > 0;
#else
    return false;
#endif
}

} // namespace

// The file holds the user's intent; the registry is a projection of it that has to
// be kept in step. An uninstaller that deleted the Run key, or an install that moved
// to another folder, leaves the projection missing or stale, and reading that as
// "off" is what used to freeze "off" into the file on the next save. A verb that
// belongs to another program is never taken over.
bool AppPrefs::ReconcileRegistryWithFile() {
    bool adopted = false;

    const std::wstring run_value = ReadRegString(RegPath(kRunKey), kRunValue);
    const bool run_is_ours = CommandIsPulse(run_value);
    if (launch_on_startup) {
        // Missing, or naming a Pulse that is no longer this executable.
        if (run_value.empty() || (run_is_ours && run_value != LaunchOnStartupCommand()))
            ApplyLaunchOnStartup(true);
    } else if (run_is_ours) {
        // The Run key has no "present but off" state: a value that is ours means the
        // user wanted auto-start on and the file lost it (restored profile, another
        // install copy). Adopt it instead of switching the setting off silently.
        launch_on_startup = true;
        adopted = true;
    }

    const std::wstring exe = ExePath();
    bool associations_changed = false;
    for (const wchar_t* cls : kFolderOpenClasses) {
        const std::wstring command = ReadRegString(FolderOpenKey(cls) + L"\\command");
        const bool ours = CommandIsPulse(command);
        const bool another_owner = !ours && !command.empty();
        if (open_folders_in_pulse) {
            // Strict here: "configured" means this executable answers the verb right
            // now, so a value naming an older path is rewritten rather than kept.
            if (!FolderOpenClassIsConfigured(cls, exe) && !another_owner) {
                WriteFolderOpenClass(cls, exe);
                associations_changed = true;
            }
        } else if (FolderOpenClassIsConfigured(cls, exe)) {
            // Reverse protection, same as the Run key: the association works, so the
            // file lost the "on" and must not turn it off behind the user's back.
            open_folders_in_pulse = true;
            adopted = true;
        } else if (FolderOpenClassNeedsClear(cls)) {
            // Ours but unusable: a command without the open verb, a path left by an
            // install that moved, or only the verb an interrupted cleanup left behind.
            // ClearFolderOpenClass refuses anything another program owns, so this never
            // reaches past our own residue.
            ClearFolderOpenClass(cls);
            associations_changed = true;
        }
    }
    if (associations_changed) NotifyAssocChanged();
    return adopted;
}

bool AppPrefs::Load() {
    // A redirected data directory means the run must not touch the user's real state:
    // these toggles live in HKCU, and a repaired verb would point the machine at
    // whatever executable is asking. A sandboxed copy of those keys is fair game.
    const std::wstring sandbox = RegistrySandboxPrefix();
    const bool allow_registry = !sandbox.empty() || !DataDirRedirected();
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        if (allow_registry) {
            launch_on_startup = ReadLaunchOnStartup();
            open_folders_in_pulse = ReadFolderOpen();
        }
        // Nothing was read, but these are the values this process runs with: the
        // next Save() must treat them as the file's state, not as a change.
        disk_state_ = *this;
        loaded_from_file_ = false;
        return false;
    }
    AppPrefsValues disk;
    bool main_exists = false;
    loaded_from_file_ = ReadDiskState(disk, main_exists);
    if (loaded_from_file_) static_cast<AppPrefsValues&>(*this) = disk;
    if (persist && allow_registry) {
        if (loaded_from_file_) {
            // Baseline for the merge below and for the write the reconciliation may
            // ask for: the file's own values, before anything is adopted back.
            disk_state_ = *this;
            if (ReconcileRegistryWithFile()) Save();
        } else {
            // First migration: no usable file, so the machine's current state is all
            // there is to inherit.
            launch_on_startup = ReadLaunchOnStartup();
            open_folders_in_pulse = ReadFolderOpen();
        }
    }
    // The state to merge against is what this process runs with, not what the file
    // happened to hold: a repair above must never read as a local change later.
    disk_state_ = *this;
    return true;
}

bool AppPrefs::Save() const {
    if (!persist) return true;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    const std::wstring path = dir + L"\\app.json";

    AppPrefsValues disk;
    bool main_exists = false;
    bool used_backup = false;
    const bool disk_known = ReadDiskState(disk, main_exists, &used_backup);

    bool quarantined = false;
    if (!disk_known && main_exists) {
        // The file is there but neither it nor its backup can be read: writing over
        // it would destroy the only copy of whatever it holds. Keep those bytes as
        // app.json.bad and start over. When even the rename fails the file is held
        // open by another process, and leaving it untouched is the safe outcome.
        if (!QuarantineUnreadableFile(path)) return false;
        quarantined = true;
    }

    // Nothing readable in either file: every local value is written as it stands.
    AppPrefs out = *this;
    if (disk_known) static_cast<AppPrefsValues&>(out) = MergedWithDisk(disk);

    // Keep the previous file one step back: a later Load() falls back to it when the
    // main file is unreadable or truncated. The file that just became the .bad
    // evidence is gone already, and a main file we could not trust must not replace
    // the backup that just saved the settings. The policy has a single copy in
    // utf8_file.h (KeepPreviousFileCopy), shared with context_menu.json.
    if (!quarantined && main_exists && !used_backup) KeepPreviousFileCopy(path);

    if (!WriteUtf8FileAtomic(path, out.ToJson())) return false;
    // The next merge compares against what this process holds, not against what was
    // just written: a field adopted from the file must not look like a local change.
    disk_state_ = *this;
    return true;
}

} // namespace pulse::app
