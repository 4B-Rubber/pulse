#include "update_download_cleanup.h"
#include "../common/current_user_security.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <array>
#include <cstring>
#include <thread>
#include <vector>
#include <algorithm>

namespace pulse::app {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void Close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
};
struct Identity {
    DWORD volume = 0, high = 0, low = 0;
    uint64_t created = 0;
    bool operator==(const Identity&) const = default;
};
struct Receipt {
    uint64_t magic = 0x3154504352455550ull; // PURECPT1
    DWORD version = 1, creator = 0;
    uint64_t created = 0, process_created = 0;
    Identity root, directory, file;
};
uint64_t Ticks(FILETIME time) { return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
uint64_t Now() { FILETIME time{}; GetSystemTimeAsFileTime(&time); return Ticks(time); }
uint64_t ProcessTime(HANDLE process) {
    FILETIME created{}, exit{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &exit, &kernel, &user) ? Ticks(created) : 0;
}
bool Info(HANDLE handle, bool directory, Identity& id) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory || (!directory && info.nNumberOfLinks != 1)) return false;
    id = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow, Ticks(info.ftCreationTime)}; return true;
}
HANDLE OpenDirectory(const std::wstring& path, bool deletable = false) {
    return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | (deletable ? DELETE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}
bool ReadReceipt(const std::wstring& path, Receipt& receipt, Handle& file, bool deletable) {
    file.value = CreateFileW(path.c_str(), GENERIC_READ | (deletable ? DELETE : 0), FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Identity unused;
    LARGE_INTEGER size{};
    if (file.value == INVALID_HANDLE_VALUE || !Info(file.value, false, unused) ||
        !GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || size.QuadPart > 4096) return false;
    std::vector<BYTE> encrypted(static_cast<size_t>(size.QuadPart)); DWORD got = 0;
    if (!ReadFile(file.value, encrypted.data(), static_cast<DWORD>(encrypted.size()), &got, nullptr) || got != encrypted.size()) return false;
    DATA_BLOB input{got, encrypted.data()}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    const bool valid = output.cbData == sizeof(Receipt);
    if (valid) memcpy(&receipt, output.pbData, sizeof(receipt));
    LocalFree(output.pbData);
    return valid && receipt.magic == 0x3154504352455550ull && receipt.version == 1;
}
bool WriteReceipt(const std::wstring& path, const Receipt& receipt) {
    DATA_BLOB input{sizeof(Receipt), reinterpret_cast<BYTE*>(const_cast<Receipt*>(&receipt))}, output{};
    if (!CryptProtectData(&input, L"Pulse update download ownership", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
    DWORD written = 0;
    const bool ok = file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, output.pbData, output.cbData, &written, nullptr) &&
        written == output.cbData && FlushFileBuffers(file.value);
    LocalFree(output.pbData);
    if (!ok && file.value != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO remove{TRUE}; SetFileInformationByHandle(file.value, FileDispositionInfo, &remove, sizeof(remove));
    }
    return ok;
}
bool Name(std::wstring_view name) {
    return name.size() == 32 && std::all_of(name.begin(), name.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
}
bool Root(const std::wstring& root, Handle& directory, Identity& identity, bool create) {
    bool created = false;
    if (create) {
        CurrentUserSecurityAttributes security;
        if (!security) return false;
        created = CreateDirectoryW(root.c_str(), security.get()) != FALSE;
        if (!created && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    directory.value = OpenDirectory(root);
    if (directory.value == INVALID_HANDLE_VALUE || !Info(directory.value, true, identity)) return false;
    if (created) { Receipt marker; marker.root = identity; if (!WriteReceipt(root + L"\\owner.dat", marker)) return false; }
    Receipt marker; Handle owner;
    return ReadReceipt(root + L"\\owner.dat", marker, owner, false) && marker.root == identity;
}
bool CreatorAlive(const Receipt& receipt) {
    if (!receipt.creator || !receipt.process_created) return true;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, receipt.creator)};
    if (!process.value || process.value == INVALID_HANDLE_VALUE) return GetLastError() != ERROR_INVALID_PARAMETER;
    const auto creation = ProcessTime(process.value);
    if (!creation) return true;
    return creation == receipt.process_created && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT;
}
bool Dispose(HANDLE file) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
}
// 0 retained (busy/live), 1 removed, 2 rejected ownership/layout.
unsigned CollectOne(const std::wstring& root, const Identity& root_id, const std::wstring& name,
                    uint64_t minimum_age_ms, bool own_creator) {
    if (!Name(name)) return 2;
    const auto path = root + L"\\" + name;
    Handle directory{OpenDirectory(path, true)}; Identity id;
    if (directory.value == INVALID_HANDLE_VALUE) return 0;
    if (!Info(directory.value, true, id)) return 2;
    Receipt receipt; Handle proof;
    if (!ReadReceipt(path + L"\\receipt.dat", receipt, proof, true) || !(receipt.root == root_id) || !(receipt.directory == id)) return 2;
    const bool own = own_creator && receipt.creator == GetCurrentProcessId() && receipt.process_created == ProcessTime(GetCurrentProcess());
    if (!own && (CreatorAlive(receipt) || Now() < receipt.created || (Now() - receipt.created) / 10000 < minimum_age_ms)) return 0;
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) return 2;
    bool layout = true, has_file = false;
    do {
        const std::wstring_view entry(item.cFileName);
        if (entry == L"." || entry == L"..") continue;
        if ((item.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            (entry != L"receipt.dat" && entry != L"PulseSetup.exe")) { layout = false; break; }
        if (entry == L"PulseSetup.exe") has_file = true;
    } while (FindNextFileW(search, &item));
    const DWORD enumeration_error = GetLastError(); FindClose(search);
    if (!layout || enumeration_error != ERROR_NO_MORE_FILES) return 2;
    if (has_file) {
        Handle installer{CreateFileW((path + L"\\PulseSetup.exe").c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (installer.value == INVALID_HANDLE_VALUE) return 0;
        Identity file_id;
        if (!Info(installer.value, false, file_id) || !(file_id == receipt.file)) return 2;
        if (!Dispose(installer.value)) return 0;
        installer.Close();
    }
    // Missing installer is valid crash recovery after its successful disposition.
    if (!Dispose(proof.value)) return 0;
    proof.Close();
    return Dispose(directory.value) ? 1u : 0u;
}
}
bool CreateUpdateDownloadPackage(const std::wstring& root, UpdateDownloadPackage& package) {
    package = {};
    Handle parent; Identity root_id;
    if (!Root(root, parent, root_id, true)) return false;
    std::array<uint8_t, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    std::wstring name; constexpr wchar_t digits[] = L"0123456789abcdef";
    for (auto byte : random) { name += digits[byte >> 4]; name += digits[byte & 15]; }
    const auto directory_path = root + L"\\" + name;
    CurrentUserSecurityAttributes security;
    if (!security || !CreateDirectoryW(directory_path.c_str(), security.get())) return false;
    Handle directory{OpenDirectory(directory_path, true)};
    Receipt receipt; receipt.root = root_id; receipt.creator = GetCurrentProcessId();
    receipt.process_created = ProcessTime(GetCurrentProcess()); receipt.created = Now();
    const auto file_path = directory_path + L"\\PulseSetup.exe";
    Handle installer{CreateFileW(file_path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    const bool ready = directory.value != INVALID_HANDLE_VALUE && Info(directory.value, true, receipt.directory) &&
        installer.value != INVALID_HANDLE_VALUE && Info(installer.value, false, receipt.file) &&
        receipt.process_created && WriteReceipt(directory_path + L"\\receipt.dat", receipt);
    if (!ready) {
        if (installer.value != INVALID_HANDLE_VALUE) Dispose(installer.value);
        installer.Close();
        if (directory.value != INVALID_HANDLE_VALUE) Dispose(directory.value);
        return false;
    }
    installer.Close(); directory.Close();
    package = {root, directory_path, file_path}; return true;
}
HANDLE OpenUpdateDownloadPackage(const UpdateDownloadPackage& package, DWORD access, DWORD sharing) {
    const auto slash = package.directory.find_last_of(L'\\');
    if (slash == std::wstring::npos || package.directory.substr(0, slash) != package.root ||
        !Name(package.directory.substr(slash + 1)) || package.file != package.directory + L"\\PulseSetup.exe") return INVALID_HANDLE_VALUE;
    Handle parent; Identity root_id;
    if (!Root(package.root, parent, root_id, false)) return INVALID_HANDLE_VALUE;
    Handle directory{OpenDirectory(package.directory)}; Identity directory_id;
    Receipt receipt; Handle proof;
    if (directory.value == INVALID_HANDLE_VALUE || !Info(directory.value, true, directory_id) ||
        !ReadReceipt(package.directory + L"\\receipt.dat", receipt, proof, false) ||
        !(receipt.root == root_id) || !(receipt.directory == directory_id)) return INVALID_HANDLE_VALUE;
    Handle file{CreateFileW(package.file.c_str(), access | FILE_READ_ATTRIBUTES, sharing, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    Identity file_id;
    if (file.value == INVALID_HANDLE_VALUE || !Info(file.value, false, file_id) || !(file_id == receipt.file)) return INVALID_HANDLE_VALUE;
    const HANDLE result = file.value; file.value = INVALID_HANDLE_VALUE; return result;
}
bool RemoveUpdateDownloadPackage(const UpdateDownloadPackage& package) {
    if (package.directory.empty()) return true;
    const auto slash = package.directory.find_last_of(L'\\');
    if (slash == std::wstring::npos || package.directory.substr(0, slash) != package.root) return false;
    Handle parent; Identity identity;
    return Root(package.root, parent, identity, false) && CollectOne(package.root, identity, package.directory.substr(slash + 1), 0, true) == 1;
}
UpdateCleanupResult CollectUpdateDownloads(const std::wstring& root, UpdateCleanupOptions options) {
    UpdateCleanupResult result;
    options.attempts = (std::min)(options.attempts, 64u); options.delay_ms = (std::min)(options.delay_ms, 2000u);
    for (unsigned pass = 0; pass < options.attempts; ++pass) {
        Handle parent; Identity identity;
        if (!Root(root, parent, identity, false)) { ++result.rejected; break; }
        WIN32_FIND_DATAW item{}; HANDLE search = FindFirstFileW((root + L"\\*").c_str(), &item);
        if (search == INVALID_HANDLE_VALUE) break;
        std::vector<std::wstring> names;
        unsigned scanned = 0;
        do {
            if (++scanned > 4096) break;
            if (Name(item.cFileName) && names.size() < 64) names.emplace_back(item.cFileName);
        } while (FindNextFileW(search, &item));
        const DWORD error = GetLastError(); FindClose(search);
        if (scanned > 4096 || error != ERROR_NO_MORE_FILES) { ++result.rejected; break; }
        unsigned pending = 0;
        for (const auto& name : names) {
            const auto outcome = CollectOne(root, identity, name, options.minimum_age_ms, false);
            if (outcome == 1) ++result.removed;
            else if (outcome == 2) ++result.rejected;
            else ++pending;
        }
        result.retained = pending;
        if (!pending || pass + 1 == options.attempts) break;
        Sleep(options.delay_ms);
    }
    return result;
}
void StartUpdateDownloadCleanup() {
    try {
        std::thread([] {
            try {
                wchar_t local[MAX_PATH]{};
                if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local)))
                    CollectUpdateDownloads(std::wstring(local) + L"\\PulseUpdateDownloads");
            } catch (...) { /* A future startup retries the same durable receipts. */ }
        }).detach();
    } catch (...) { /* The immutable receipt remains available on the next startup. */ }
}
}
