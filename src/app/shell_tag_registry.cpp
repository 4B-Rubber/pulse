#include "shell_tag_registry.h"
#include "shell_tag_protocol.h"
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse::app {
namespace {
constexpr const wchar_t* parents[] = {
    L"Software\\Classes\\*\\shell\\PulseTags",
    L"Software\\Classes\\Directory\\shell\\PulseTags",
};
bool SetString(HKEY key, const wchar_t* name, const std::wstring& value, const TagMenuRegistryApi& api) {
    return api.set_value(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool CreateKeyWith(const std::wstring& path,
                   std::initializer_list<std::pair<const wchar_t*, std::wstring>> values, const TagMenuRegistryApi& api) {
    HKEY h = nullptr;
    if (api.create_key(api.root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = true;
    for (const auto& [name, value] : values) ok = SetString(h, name, value, api) && ok;
    RegCloseKey(h);
    return ok;
}

// 32-bpp ICO with an anti-aliased dot in the tag color, 16/20/24/32 px.
bool WriteDotIcon(const std::wstring& file, uint32_t rgb, const TagMenuRegistryApi& api) {
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
    HANDLE h = api.create_file(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) &&
                    written == out.size();
    CloseHandle(h);
    return ok;
}

bool RemoveIcons(const std::wstring& dir) {
    if (dir.empty()) return true;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.ico").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    bool ok = true;
    do {
        ok = DeleteFileW((dir + L"\\" + fd.cFileName).c_str()) != FALSE && ok;
    } while (FindNextFileW(find, &fd));
    const DWORD error = GetLastError();
    FindClose(find);
    return ok && error == ERROR_NO_MORE_FILES;
}


bool ValueMatches(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& expected) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) return false;
    DWORD type = 0, bytes = 0;
    auto result = RegQueryValueExW(handle, name, nullptr, &type, nullptr, &bytes);
    std::wstring value(expected.size() + 1, L'\0');
    if (result == ERROR_SUCCESS && type == REG_SZ && bytes == (expected.size() + 1) * sizeof(wchar_t))
        result = RegQueryValueExW(handle, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
    else result = ERROR_INVALID_DATA;
    RegCloseKey(handle);
    return result == ERROR_SUCCESS && value == expected + L'\0';
}
}

bool RemoveTagMenu(const std::wstring& dir, const TagMenuRegistryApi& api) {
    bool ok = true;
    for (const auto* parent : parents) {
        const LSTATUS error = api.delete_key(api.root, parent);
        ok = (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) && ok;
    }
    const auto removed = api.delete_key(api.root, shell_tags::kRegistryRoot);
    ok = (removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND || removed == ERROR_PATH_NOT_FOUND) && ok;
    if (!ok) return false;
    ok = RemoveIcons(dir);
    if (!dir.empty() && !RemoveDirectoryW(dir.c_str())) {
        const DWORD error = GetLastError();
        ok = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) && ok;
    }
    return ok;
}

bool TagMenuMatches(const std::vector<ColorTag>& tags, const std::wstring& exe,
                    const std::wstring& title, const std::wstring& dir, HKEY registry_root) {
    const auto matches = [registry_root](const std::wstring& key, const wchar_t* name, const std::wstring& value) {
        return ValueMatches(registry_root, key, name, value);
    };
    const std::wstring server = std::wstring(shell_tags::kRegistryRoot) + L"\\LocalServer32";
    if (!matches(server, nullptr, L"\"" + exe + L"\" --shell-tag-com") ||
        !matches(server, L"ServerExecutable", exe)) return false;
    for (const auto* parent : parents) {
        if (!matches(parent, L"MUIVerb", title) || !matches(parent, L"Icon", exe + L",0") ||
            !matches(parent, L"SubCommands", L"") ||
            !matches(parent, L"MultiSelectModel", L"Player") ||
            !matches(std::wstring(parent) + L"\\command", L"DelegateExecute", shell_tags::kClassId) ||
            !matches(std::wstring(parent) + L"\\command", nullptr, L"")) return false;
        for (const auto& tag : tags) {
            if (tag.id.empty() || tag.name.empty()) continue;
            if (!shell_tags::SafeTagId(tag.id)) return false;
            const std::wstring verb = std::wstring(parent) + L"\\shell\\" + shell_tags::VerbName(tag.id);
            const std::wstring icon = dir + L"\\" + tag.id + L".ico";
            if (!matches(verb, L"MUIVerb", tag.name) || !matches(verb, L"MultiSelectModel", L"Player") ||
                !matches(verb, L"Icon", icon) || GetFileAttributesW(icon.c_str()) == INVALID_FILE_ATTRIBUTES ||
                !matches(verb + L"\\command", L"DelegateExecute", shell_tags::kClassId) ||
                !matches(verb + L"\\command", nullptr, L"")) return false;
        }
    }
    return true;
}

bool InstallTagMenu(const std::vector<ColorTag>& tags, const std::wstring& exe,
                    const std::wstring& title, const std::wstring& dir, const TagMenuRegistryApi& api) {
    if (exe.empty() || dir.empty()) return false;
    for (const auto& tag : tags) if (!tag.id.empty() && !tag.name.empty() && !shell_tags::SafeTagId(tag.id)) return false;
    if (!RemoveTagMenu(dir, api)) return false;
    const int created = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS) return false;
    bool ok = CreateKeyWith(std::wstring(shell_tags::kRegistryRoot) + L"\\LocalServer32",
        {{nullptr, L"\"" + exe + L"\" --shell-tag-com"}, {L"ServerExecutable", exe}}, api);
    for (const auto* parent : parents) {
        const std::wstring root = parent;
        ok = CreateKeyWith(root, {{L"MUIVerb", title}, {L"Icon", exe + L",0"},
            {L"SubCommands", L""}, {L"MultiSelectModel", L"Player"}}, api) && ok;
        // Mark the cascading parent as COM-backed too; otherwise Shell applies
        // the legacy Player limit (100 items) before it examines child verbs.
        ok = CreateKeyWith(root + L"\\command", {{nullptr, L""}, {L"DelegateExecute", shell_tags::kClassId}}, api) && ok;
        for (const auto& tag : tags) {
            if (tag.id.empty() || tag.name.empty()) continue;
            const std::wstring icon = dir + L"\\" + tag.id + L".ico";
            if (GetFileAttributesW(icon.c_str()) == INVALID_FILE_ATTRIBUTES && !WriteDotIcon(icon, tag.rgb, api)) ok = false;
            const std::wstring verb = root + L"\\shell\\" + shell_tags::VerbName(tag.id);
            ok = CreateKeyWith(verb, {{L"MUIVerb", tag.name}, {L"MultiSelectModel", L"Player"}, {L"Icon", icon}}, api) && ok;
            ok = CreateKeyWith(verb + L"\\command", {{nullptr, L""}, {L"DelegateExecute", shell_tags::kClassId}}, api) && ok;
        }
    }
    return ok && TagMenuMatches(tags, exe, title, dir, api.root);
}
}
