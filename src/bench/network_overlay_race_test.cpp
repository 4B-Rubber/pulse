#include <windows.h>
#include <winnetwk.h>
#include "../index/network_index.h"
#include "../index/index_config.h"
#include <filesystem>
#include <functional>
#include <iostream>

namespace {
const std::wstring root_path = L"\\\\pulse-test.invalid\\race";
const std::wstring item_path = root_path + L"\\item.txt";
std::wstring fixture_directory;
uint32_t file_size = 10;
std::function<void()> after_enumeration;
int handles_open = 0;
bool unexpected_path = false;
HANDLE WINAPI RaceFindFirst(LPCWSTR path, FINDEX_INFO_LEVELS, LPVOID data,
                            FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    if (std::wstring(path) != L"\\\\?\\UNC\\pulse-test.invalid\\race\\*" &&
        std::wstring(path) != L"\\\\?\\UNC\\pulse-test.invalid\\race\\parent\\*") {
        unexpected_path = true;
        SetLastError(ERROR_BAD_NETPATH);
        return INVALID_HANDLE_VALUE;
    }
    auto& find = *static_cast<WIN32_FIND_DATAW*>(data);
    find = {};
    find.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    find.nFileSizeLow = file_size;
    wcscpy_s(find.cFileName, L"item.txt");
    ++handles_open;
    return reinterpret_cast<HANDLE>(new int(1));
}
BOOL WINAPI RaceFindNext(HANDLE, LPWIN32_FIND_DATAW) {
    if (after_enumeration) {
        auto callback = std::move(after_enumeration);
        after_enumeration = {};
        callback();
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}
BOOL WINAPI RaceFindClose(HANDLE handle) {
    delete reinterpret_cast<int*>(handle);
    --handles_open;
    return TRUE;
}
BOOL WINAPI RaceAttributes(LPCWSTR path, GET_FILEEX_INFO_LEVELS, LPVOID data) {
    if (std::wstring(path) != L"\\\\?\\UNC\\pulse-test.invalid\\race\\item.txt") {
        unexpected_path = true;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    auto& attributes = *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(data);
    attributes = {};
    attributes.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    attributes.nFileSizeLow = file_size;
    return TRUE;
}
}
namespace pulse::index {
std::wstring RaceUserIndexRoot() { return fixture_directory; }
}
#define FindFirstFileExW RaceFindFirst
#define FindNextFileW RaceFindNext
#define FindClose RaceFindClose
#define GetFileAttributesExW RaceAttributes
#define UserIndexRoot RaceUserIndexRoot
#include "../index/network_index.cpp"
#undef UserIndexRoot
#undef GetFileAttributesExW
#undef FindClose
#undef FindNextFileW
#undef FindFirstFileExW

namespace pulse::index {
struct NetworkIndexTestAccess {
    static void Prepare(NetworkIndex& index) {
        index.running_ = true;
        NetworkIndex::RootState root;
        root.info.path = root_path;
        root.overlay = std::make_shared<NetworkIndex::Overlay>();
        index.roots_.push_back(std::move(root));
    }
    static void Watch(NetworkIndex& index) { index.SetWatching(root_path, true); }
    static void LoseWatch(NetworkIndex& index) { index.SetWatching(root_path, false); }
    static void Build(NetworkIndex& index) { index.BuildRoot(root_path, index.generation_); }
    static bool Pending(NetworkIndex& index) { return index.dirty_roots_.contains(root_path); }
    static bool Waiting(NetworkIndex& index) {
        return index.roots_.front().info.state == L"监视已启动 · 等待校验";
    }
    static bool HasShard(NetworkIndex& index, uint64_t size) {
        const auto& shard = index.roots_.front().shard;
        return shard && shard->count == 1 && shard->records[0].size == size;
    }
    static bool NoShard(NetworkIndex& index) { return !index.roots_.front().shard; }
    static void Notify(NetworkIndex& index, DWORD action, const std::wstring& name = L"item.txt") {
        const size_t bytes = offsetof(FILE_NOTIFY_INFORMATION, FileName) + name.size() * sizeof(wchar_t);
        std::vector<DWORD> buffer((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
        auto* notification = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());
        notification->Action = action;
        notification->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        memcpy(notification->FileName, name.data(), notification->FileNameLength);
        index.ObserveChanges(root_path, reinterpret_cast<const BYTE*>(buffer.data()), static_cast<DWORD>(bytes));
    }
    static void Scan(NetworkIndex& index, const std::wstring& directory = root_path) {
        index.roots_.front().overlay->pending_scans.push_back(directory);
        index.ScanPendingSubtrees();
    }
    static bool Entry(NetworkIndex& index, uint64_t size) {
        const auto view = index.OverlayViewLocked(index.roots_.front());
        return view && view->added->count == 1 && view->added->records[0].size == size;
    }
    static bool Removed(NetworkIndex& index) {
        auto& root = index.roots_.front();
        const auto view = index.OverlayViewLocked(root);
        return !root.overlay->entries.contains(item_path) && view && view->added->count == 0;
    }
    static void Readd(NetworkIndex& index) {
        index.roots_.clear();
        ++index.generation_;
        Prepare(index);
    }
    static bool EmptyOverlay(NetworkIndex& index) {
        const auto& overlay = *index.roots_.front().overlay;
        return overlay.entries.empty() && overlay.removed.empty() && !index.roots_.front().change_pending;
    }
    static bool ParentRemoved(NetworkIndex& index) {
        const auto& overlay = *index.roots_.front().overlay;
        return overlay.entries.empty() && overlay.removed.contains(root_path + L"\\parent");
    }
};
}

int main() {
    using namespace pulse::index;
    using Access = NetworkIndexTestAccess;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        failures += !ok;
    };
    const auto temp = std::filesystem::temp_directory_path();
    const auto fixture = temp / (L"pulse-network-race-test-" + std::to_wstring(GetCurrentProcessId()) +
                                L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    if (!std::filesystem::create_directory(fixture, error)) return 1;
    fixture_directory = fixture.wstring();
    {
        NetworkIndex index;
        Access::Prepare(index);
        file_size = 10;
        after_enumeration = [&] { file_size = 99; Access::Watch(index); };
        Access::Build(index);
        check(Access::NoShard(index) && Access::Pending(index) &&
              !std::filesystem::exists(ShardPath(root_path, 0)),
              "M06-003 watch established after enumeration prevents stale first publication");
        Access::Build(index);
        check(Access::HasShard(index, 99), "M06-003 compensating watched baseline publishes current data");
    }
    {
        NetworkIndex index;
        Access::Prepare(index);
        file_size = 10;
        Access::Build(index);
        check(Access::HasShard(index, 10), "M06-003 server without watch support still publishes baseline");
        file_size = 99;
        Access::Watch(index);
        check(Access::Pending(index) && Access::Waiting(index),
              "M06-003 watch established after publication waits for compensation");
        Access::Build(index);
        check(Access::HasShard(index, 99), "M06-003 post-publication compensation replaces stale snapshot");
        Access::LoseWatch(index);
        file_size = 10;
        after_enumeration = [&] { file_size = 77; Access::Watch(index); };
        Access::Build(index);
        check(Access::HasShard(index, 99) && Access::Pending(index),
              "M06-003 late watch during rebuild retains old snapshot");
        Access::Build(index);
        check(Access::HasShard(index, 77), "M06-003 watched retry replaces retained snapshot");
        index.Stop();
    }
    {
        NetworkIndex index;
        Access::Prepare(index);
        file_size = 10;
        Access::Watch(index);
        after_enumeration = [&] { file_size = 99; Access::Notify(index, FILE_ACTION_MODIFIED); };
        Access::Build(index);
        check(Access::HasShard(index, 10) && Access::Entry(index, 99),
              "M06-003 watch update after enumeration supersedes first baseline");
    }
    for (int scenario = 0; scenario != 4; ++scenario) {
        NetworkIndex index;
        Access::Prepare(index);
        file_size = 10;
        after_enumeration = [&] {
            if (scenario == 3) { Access::Readd(index); return; }
            if (scenario != 0) Access::Notify(index, FILE_ACTION_REMOVED);
            if (scenario != 2) {
                file_size = 99;
                Access::Notify(index, scenario == 1 ? FILE_ACTION_ADDED : FILE_ACTION_MODIFIED);
            }
        };
        Access::Scan(index);
        check(scenario == 3 ? Access::EmptyOverlay(index) : scenario == 2 ? Access::Removed(index) : Access::Entry(index, 99),
              scenario == 0 ? "M06-004 subtree scan preserves newer watched metadata" :
              scenario == 1 ? "M06-004 subtree scan preserves delete/recreate result" :
              scenario == 2 ? "M06-004 subtree scan preserves newer tombstone" :
                              "M06-004 removed and re-added root rejects old scan result");
    }
    {
        NetworkIndex index;
        Access::Prepare(index);
        file_size = 10;
        after_enumeration = [&] { Access::Notify(index, FILE_ACTION_REMOVED, L"parent"); };
        Access::Scan(index, root_path + L"\\parent");
        check(Access::ParentRemoved(index), "M06-004 newer ancestor tombstone blocks scanned descendants");
    }
    check(handles_open == 0 && !unexpected_path, "all enumerations closed; no real network paths accessed");
    std::error_code guard_error;
    const bool same_parent = std::filesystem::equivalent(fixture.parent_path(), temp, guard_error);
    const auto status = std::filesystem::symlink_status(fixture, error);
    const bool owned_directory = same_parent && !guard_error && !error &&
        status.type() == std::filesystem::file_type::directory &&
        fixture.filename().wstring().starts_with(L"pulse-network-race-test-");
    if (owned_directory) std::filesystem::remove_all(fixture, error);
    check(owned_directory && !error && !std::filesystem::exists(fixture), "private fixture cleaned up");
    return failures ? 1 : 0;
}
