// shell_tag_menu.cpp - File Explorer "Pulse tags >" submenu (see header).
#include "shell_tag_menu.h"
#include "app_internal.h"
#include "../common/localization.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace pulse {
bool ToggleTagForSelection(AppState& s, const app::TagId& tag_id,
                           const std::vector<std::wstring>& paths);
}

namespace pulse::app {
namespace {

constexpr ULONG_PTR kShellTagMessage = 0x50544147; // 'PTAG'
constexpr wchar_t kMainWindowClass[] = L"PulseMainWindow";
constexpr size_t kMaxRequestChars = 32768;

} // namespace

std::optional<ShellTagRequest> ParseShellTagArgs(int argc, wchar_t** argv) {
    for (int i = 1; i + 2 < argc; ++i) {
        if (wcscmp(argv[i], L"--tag") != 0) continue;
        ShellTagRequest request{argv[i + 1], argv[i + 2]};
        if (request.tag_id.empty() || request.path.empty()) return std::nullopt;
        return request;
    }
    return std::nullopt;
}

bool ForwardShellTagRequest(const ShellTagRequest& request) {
    HWND hwnd = nullptr;
    // A multi-selection starts one process per item; the first one becomes
    // the primary instance and may still be creating its window.
    for (int i = 0; i < 100 && !hwnd; ++i) {
        hwnd = FindWindowW(kMainWindowClass, nullptr);
        if (!hwnd) Sleep(50);
    }
    if (!hwnd) return false;
    std::wstring payload = request.tag_id;
    payload.push_back(L'\n');
    payload += request.path;
    if (payload.size() >= kMaxRequestChars) return false;
    COPYDATASTRUCT data{};
    data.dwData = kShellTagMessage;
    data.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
    data.lpData = payload.data();
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                               SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result) != 0 &&
           result != FALSE;
}

bool DecodeShellTagRequest(const COPYDATASTRUCT* data, ShellTagRequest& out) {
    if (!data || data->dwData != kShellTagMessage || !data->lpData ||
        data->cbData < 2 * sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0 ||
        data->cbData > kMaxRequestChars * sizeof(wchar_t))
        return false;
    const auto* text = static_cast<const wchar_t*>(data->lpData);
    const size_t count = data->cbData / sizeof(wchar_t);
    if (text[count - 1] != L'\0') return false;
    const std::wstring payload(text, count - 1);
    const size_t split = payload.find(L'\n');
    if (split == std::wstring::npos || split == 0 || split + 1 >= payload.size()) return false;
    out.tag_id = payload.substr(0, split);
    out.path = payload.substr(split + 1);
    return true;
}

} // namespace pulse::app

namespace pulse {
namespace {

constexpr ULONGLONG kBatchWindowMs = 400;       // Explorer's per-item launches
constexpr ULONGLONG kHeadlessExitDelayMs = 1500; // let ADS writes drain
constexpr ULONGLONG kRegistrySyncPeriodMs = 2000;
constexpr const wchar_t* kVerbParents[] = {
    L"Software\\Classes\\*\\shell\\PulseTags",
    L"Software\\Classes\\Directory\\shell\\PulseTags",
};

struct ShellTagState {
    std::map<std::wstring, std::vector<std::wstring>> pending; // tag id -> paths
    ULONGLONG last_request = 0;
    bool headless = false;
    ULONGLONG headless_exit_at = 0;
    ULONGLONG next_sync = 0;
    size_t synced_signature = 0;
    bool synced_once = false;
};

ShellTagState& State() {
    static ShellTagState state;
    return state;
}

std::wstring ModulePath() {
    wchar_t path[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return n > 0 && n < ARRAYSIZE(path) ? std::wstring(path, n) : std::wstring();
}

std::wstring IconDirectory() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
        dir = std::wstring(base) + L"\\Pulse\\tagicons";
    CoTaskMemFree(base);
    return dir;
}

bool SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool CreateKeyWith(const std::wstring& path,
                   std::initializer_list<std::pair<const wchar_t*, std::wstring>> values) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = true;
    for (const auto& [name, value] : values) ok = SetString(h, name, value) && ok;
    RegCloseKey(h);
    return ok;
}

// 32-bpp ICO with an anti-aliased dot in the tag color, 16/20/24/32 px.
bool WriteDotIcon(const std::wstring& file, uint32_t rgb) {
    const int sizes[] = {16, 20, 24, 32};
    std::vector<std::vector<uint8_t>> images;
    for (int size : sizes) {
        std::vector<uint8_t> image(sizeof(BITMAPINFOHEADER));
        auto* header = reinterpret_cast<BITMAPINFOHEADER*>(image.data());
        header->biSize = sizeof(BITMAPINFOHEADER);
        header->biWidth = size;
        header->biHeight = size * 2; // XOR + AND masks
        header->biPlanes = 1;
        header->biBitCount = 32;
        header->biCompression = BI_RGB;
        const float c = size * 0.5f;
        const float r = size * 0.32f;
        const uint8_t red = static_cast<uint8_t>((rgb >> 16) & 0xFF);
        const uint8_t green = static_cast<uint8_t>((rgb >> 8) & 0xFF);
        const uint8_t blue = static_cast<uint8_t>(rgb & 0xFF);
        for (int y = size - 1; y >= 0; --y) {          // bottom-up rows
            for (int x = 0; x < size; ++x) {
                const float dx = x + 0.5f - c;
                const float dy = y + 0.5f - c;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float a = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                const uint8_t alpha = static_cast<uint8_t>(std::lround(a * 255.0f));
                // Premultiplication is not used by ICO; store straight color.
                image.push_back(blue);
                image.push_back(green);
                image.push_back(red);
                image.push_back(alpha);
            }
        }
        const int mask_stride = ((size + 31) / 32) * 4;
        image.insert(image.end(), static_cast<size_t>(mask_stride * size), 0);
        images.push_back(std::move(image));
    }
    std::vector<uint8_t> out(6 + 16 * images.size());
    auto put16 = [&](size_t at, uint16_t v) { out[at] = v & 0xFF; out[at + 1] = v >> 8; };
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    put16(2, 1);
    put16(4, static_cast<uint16_t>(images.size()));
    uint32_t offset = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < images.size(); ++i) {
        const size_t e = 6 + 16 * i;
        out[e] = static_cast<uint8_t>(sizes[i]);
        out[e + 1] = static_cast<uint8_t>(sizes[i]);
        put16(e + 4, 1);
        put16(e + 6, 32);
        put32(e + 8, static_cast<uint32_t>(images[i].size()));
        put32(e + 12, offset);
        offset += static_cast<uint32_t>(images[i].size());
    }
    for (const auto& image : images) out.insert(out.end(), image.begin(), image.end());
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) &&
                    written == out.size();
    CloseHandle(h);
    return ok;
}

void RemoveIcons(const std::wstring& dir) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.ico").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

bool MenuInstalled() {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kVerbParents[0], 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return false;
    RegCloseKey(h);
    return true;
}

void RemoveMenu() {
    for (const wchar_t* parent : kVerbParents) SHDeleteKeyW(HKEY_CURRENT_USER, parent);
    const std::wstring dir = IconDirectory();
    RemoveIcons(dir);
    if (!dir.empty()) RemoveDirectoryW(dir.c_str());
}

void InstallMenu(const app::PlacesCatalog& places) {
    const std::wstring exe = ModulePath();
    if (exe.empty()) return;
    for (const wchar_t* parent : kVerbParents) SHDeleteKeyW(HKEY_CURRENT_USER, parent);
    const std::wstring dir = IconDirectory();
    if (!dir.empty()) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        RemoveIcons(dir);
    }
    const std::wstring title = l10n::Get(l10n::StringId::ShellTagMenu);
    for (const wchar_t* parent : kVerbParents) {
        const std::wstring root = parent;
        // Icon must be an unquoted "path,index": Explorer ignores the quoted
        // form for verb icons even when the path contains spaces.
        // SubCommands="" turns on the nested shell\* children; Player keeps
        // the verb visible for selections larger than 15 items.
        CreateKeyWith(root, {{L"MUIVerb", title}, {L"Icon", exe + L",0"},
                             {L"SubCommands", L""}, {L"MultiSelectModel", L"Player"}});
        int order = 0;
        for (const auto& tag : places.tags) {
            if (tag.id.empty() || tag.name.empty()) continue;
            wchar_t key[16]{};
            swprintf_s(key, L"t%03d", order++);
            std::wstring icon;
            if (!dir.empty()) {
                const std::wstring file = dir + L"\\" + tag.id + L".ico";
                if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES ||
                    WriteDotIcon(file, tag.rgb))
                    icon = file;
            }
            const std::wstring verb = root + L"\\shell\\" + key;
            CreateKeyWith(verb, {{L"MUIVerb", tag.name}, {L"MultiSelectModel", L"Player"}});
            if (!icon.empty()) CreateKeyWith(verb, {{L"Icon", icon}});
            CreateKeyWith(verb + L"\\command",
                          {{nullptr, L"\"" + exe + L"\" --tag " + tag.id + L" \"%1\""}});
        }
    }
}

size_t Signature(const AppState& s) {
    size_t h = std::hash<std::wstring>{}(ModulePath());
    auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(std::hash<std::wstring>{}(l10n::Get(l10n::StringId::ShellTagMenu)));
    for (const auto& tag : s.places.tags) {
        mix(std::hash<std::wstring>{}(tag.id));
        mix(std::hash<std::wstring>{}(tag.name));
        mix(tag.rgb);
    }
    return h;
}

void SyncRegistry(AppState& s, ULONGLONG now) {
    ShellTagState& st = State();
    if (now < st.next_sync) return;
    st.next_sync = now + kRegistrySyncPeriodMs;
    if (!s.appPrefs.persist) return;
    if (!s.appPrefs.shell_tag_menu) {
        if (!st.synced_once || st.synced_signature != 0) {
            if (MenuInstalled()) RemoveMenu();
            st.synced_signature = 0;
            st.synced_once = true;
        }
        return;
    }
    const size_t signature = Signature(s);
    if (st.synced_once && signature == st.synced_signature && MenuInstalled()) return;
    InstallMenu(s.places);
    st.synced_signature = signature;
    st.synced_once = true;
}

void ApplyDueBatches(AppState& s, ULONGLONG now) {
    ShellTagState& st = State();
    if (st.pending.empty() || now - st.last_request < kBatchWindowMs) return;
    auto pending = std::move(st.pending);
    st.pending.clear();
    for (auto& [ref, paths] : pending) {
        const app::TagId id = s.places.ResolveTagRef(ref);
        const app::ColorTag* tag = id.empty() ? nullptr : s.places.FindTag(id);
        if (!tag) continue;
        const std::wstring name = tag->name;
        const bool add = s.places.GetSelectionState(id, paths) != app::TagSelectionState::All;
        if (!ToggleTagForSelection(s, id, paths)) continue;
        wchar_t message[512]{};
        swprintf_s(message,
                   l10n::Get(add ? l10n::StringId::ShellTagAdded : l10n::StringId::ShellTagRemoved).c_str(),
                   name.c_str(), static_cast<int>(paths.size()));
        s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::ShellTagMenu), message, false);
    }
    if (st.headless) st.headless_exit_at = now + kHeadlessExitDelayMs;
}

} // namespace

void QueueShellTagRequest(AppState& s, app::ShellTagRequest request, bool headless_launch) {
    ShellTagState& st = State();
    if (headless_launch) st.headless = true;
    auto& paths = st.pending[request.tag_id];
    if (std::find(paths.begin(), paths.end(), request.path) == paths.end())
        paths.push_back(std::move(request.path));
    st.last_request = GetTickCount64();
    st.headless_exit_at = 0;
    (void)s;
}

bool ShellTagHeadlessLaunch() {
    return State().headless;
}

void TickShellTagMenu(AppState& s, ULONGLONG now) {
    ShellTagState& st = State();
    ApplyDueBatches(s, now);
    SyncRegistry(s, now);
    if (!st.headless) return;
    // The user opened the window meanwhile (Win+E, tray): stay running.
    if (s.hwnd && IsWindowVisible(s.hwnd)) {
        st.headless = false;
        return;
    }
    if (st.pending.empty() && st.headless_exit_at && now >= st.headless_exit_at && s.hwnd) {
        // headless stays set so WM_DESTROY skips the session save.
        st.headless_exit_at = 0;
        DestroyWindow(s.hwnd);
    }
}

} // namespace pulse
