#pragma once
#include "places.h"
#include <windows.h>
#include <shlwapi.h>

namespace pulse::app {
struct TagMenuRegistryApi {
    decltype(&RegCreateKeyExW) create_key = ::RegCreateKeyExW;
    decltype(&RegSetValueExW) set_value = ::RegSetValueExW;
    decltype(&SHDeleteKeyW) delete_key = ::SHDeleteKeyW;
    decltype(&CreateFileW) create_file = ::CreateFileW;
    HKEY root = HKEY_CURRENT_USER;
};
bool TagMenuMatches(const std::vector<ColorTag>& tags, const std::wstring& exe,
                    const std::wstring& title, const std::wstring& icon_dir,
                    HKEY registry_root = HKEY_CURRENT_USER);
bool InstallTagMenu(const std::vector<ColorTag>& tags, const std::wstring& exe,
                    const std::wstring& title, const std::wstring& icon_dir,
                    const TagMenuRegistryApi& api = {});
bool RemoveTagMenu(const std::wstring& icon_dir, const TagMenuRegistryApi& api = {});
}
