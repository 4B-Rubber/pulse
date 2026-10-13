#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::app {
// Only directories created with a valid, identity-bound receipt are collectible.
struct UpdateDownloadPackage {
    std::wstring root, directory, file;
};
bool CreateUpdateDownloadPackage(const std::wstring& root, UpdateDownloadPackage& package);
HANDLE OpenUpdateDownloadPackage(const UpdateDownloadPackage& package, DWORD access, DWORD sharing);
// Immediate failure/cancellation cleanup may collect its own still-live creator.
bool RemoveUpdateDownloadPackage(const UpdateDownloadPackage& package);
struct UpdateCleanupOptions {
    unsigned attempts = 64;
    unsigned delay_ms = 2000;
    uint64_t minimum_age_ms = 60000;
};
struct UpdateCleanupResult { unsigned removed = 0, retained = 0, rejected = 0; };
UpdateCleanupResult CollectUpdateDownloads(const std::wstring& root, UpdateCleanupOptions options = {});
void StartUpdateDownloadCleanup();
}
