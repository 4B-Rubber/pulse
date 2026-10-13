#include <windows.h>
#include <winnetwk.h>
#include "../index/network_index.h"
#include "../app/search_snapshot_hint.h"
#include "../index/index_config.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>

namespace {
enum class WalkCase { Normal, Empty, EmptyEof, RootDenied, ChildDenied, Interrupted };
WalkCase walk_case = WalkCase::Normal;
const std::wstring root_path = L"\\\\pulse-test.invalid\\synthetic";
std::wstring fixture_directory;
int handles_open = 0;
bool unexpected_path = false;
DWORD universal_first_result = ERROR_MORE_DATA;
DWORD universal_last_error = ERROR_SUCCESS;
DWORD universal_reported_bytes = 0;
int universal_calls = 0;

DWORD WINAPI TestGetUniversalName(LPCWSTR, DWORD level, LPVOID buffer, LPDWORD bytes) {
    ++universal_calls;
    if (level != UNIVERSAL_NAME_INFO_LEVEL || !bytes) return ERROR_INVALID_PARAMETER;
    if (!buffer) {
        *bytes = universal_reported_bytes;
        SetLastError(universal_last_error);
        return universal_first_result;
    }
    const size_t text_bytes = (root_path.size() + 1) * sizeof(wchar_t);
    const DWORD needed = static_cast<DWORD>(sizeof(UNIVERSAL_NAME_INFOW) + text_bytes);
    if (*bytes < needed) {
        *bytes = needed;
        return ERROR_MORE_DATA;
    }
    auto* info = static_cast<UNIVERSAL_NAME_INFOW*>(buffer);
    info->lpUniversalName = reinterpret_cast<wchar_t*>(static_cast<BYTE*>(buffer) + sizeof(*info));
    std::memcpy(info->lpUniversalName, root_path.c_str(), text_bytes);
    *bytes = needed;
    return NO_ERROR;
}

HANDLE WINAPI TestFindFirst(LPCWSTR path, FINDEX_INFO_LEVELS, LPVOID data,
                            FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    const std::wstring pattern(path);
    const bool root = pattern == L"\\\\?\\UNC\\pulse-test.invalid\\synthetic\\*";
    const bool child = pattern == L"\\\\?\\UNC\\pulse-test.invalid\\synthetic\\child\\*";
    if (!root && !child) {
        unexpected_path = true;
        SetLastError(ERROR_BAD_NETPATH);
        return INVALID_HANDLE_VALUE;
    }
    if (walk_case == WalkCase::RootDenied || child) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (walk_case == WalkCase::Empty) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    auto& find = *static_cast<WIN32_FIND_DATAW*>(data);
    find = {};
    const bool directory = walk_case == WalkCase::ChildDenied || walk_case == WalkCase::EmptyEof;
    find.dwFileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    const wchar_t* name = walk_case == WalkCase::ChildDenied ? L"child" :
        walk_case == WalkCase::EmptyEof ? L"." :
        walk_case == WalkCase::Interrupted ? L"partial.txt" : L"old.txt";
    wcscpy_s(find.cFileName, name);
    ++handles_open;
    return reinterpret_cast<HANDLE>(new int(1));
}

BOOL WINAPI TestFindNext(HANDLE, LPWIN32_FIND_DATAW) {
    SetLastError(walk_case == WalkCase::Interrupted ? ERROR_NETNAME_DELETED : ERROR_NO_MORE_FILES);
    return FALSE;
}

BOOL WINAPI TestFindClose(HANDLE handle) {
    delete reinterpret_cast<int*>(handle);
    --handles_open;
    // Successful calls may change last-error; callers must retain enumeration errors first.
    SetLastError(ERROR_INVALID_HANDLE);
    return TRUE;
}
} // namespace

namespace pulse::index {
std::wstring TestUserIndexRoot() { return fixture_directory; }
}

#define FindFirstFileExW TestFindFirst
#define FindNextFileW TestFindNext
#define FindClose TestFindClose
#define UserIndexRoot TestUserIndexRoot
#define WNetGetUniversalNameW TestGetUniversalName
#include "../index/network_index.cpp"
#undef WNetGetUniversalNameW
#undef UserIndexRoot
#undef FindClose
#undef FindNextFileW
#undef FindFirstFileExW

namespace pulse::index {
struct NetworkIndexTestAccess {
    static void Prepare(NetworkIndex& index) {
        index.running_ = true;
        NetworkIndex::RootState root;
        root.info.path = root_path;
        index.roots_.push_back(std::move(root));
    }
    static void Build(NetworkIndex& index) { index.BuildRoot(root_path, index.generation_); }
    static bool Ready(const NetworkIndex& index) {
        const auto& root = index.roots_.front();
        return root.shard && root.shard->count == 1 && root.info.error.empty() &&
            !root.info.building && !root.last_crawl_failed;
    }
    static const void* ShardIdentity(const NetworkIndex& index) {
        return index.roots_.front().shard.get();
    }
    static bool FailedAndRetained(const NetworkIndex& index, const void* old) {
        const auto& root = index.roots_.front();
        return root.shard.get() == old && root.shard && root.shard->count == 1 &&
            root.info.indexed_items == 1 && root.last_crawl_failed &&
            !root.info.error.empty() && !root.info.building && root.info.progress != 100 &&
            root.info.online == (walk_case != WalkCase::RootDenied);
    }
    static bool FailedCrawlRetriesWithWatch(NetworkIndex& index) {
        auto& root = index.roots_.front();
        root.info.watching = true;
        const auto due_at = root.last_crawl_end + crawl_schedule::kOfflineRetry;
        std::vector<std::wstring> due;
        const auto next = index.CollectDueRootsLocked(due_at - std::chrono::seconds(1), due);
        if (!due.empty() || next != due_at) return false;
        index.CollectDueRootsLocked(due_at, due);
        return due.size() == 1 && due.front() == root_path;
    }
    static bool FailedSubtreeKeepsOverlay(NetworkIndex& index) {
        auto& root = index.roots_.front();
        root.change_pending = false;
        root.overlay = std::make_shared<NetworkIndex::Overlay>();
        root.overlay->entries.emplace(root_path + L"\\retained.txt", NetworkIndex::Overlay::Entry{});
        root.overlay->pending_scans.push_back(root_path);
        root.info.error.clear();
        index.ScanPendingSubtrees();
        return root.overlay->entries.size() == 1 &&
            root.overlay->entries.contains(root_path + L"\\retained.txt") &&
            root.change_pending && !root.info.error.empty() && handles_open == 0;
    }
};
} // namespace pulse::index

namespace {
std::vector<char> ReadFixture(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char** argv) {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failures;
    };
    using namespace pulse::index;
    if (argc == 2 && std::string_view(argv[1]) == "--snapshot-banner") {
        NetworkRootInfo root;
        root.path = L"\\\\fixture.invalid\\share";
        root.online = true; root.indexed_items = 10;
        std::vector<NetworkRootInfo> roots{root};
        Query query;
        query.path_prefix = L"C:\\private-fixture\\source";
        check(!pulse::app::UsesNetworkSnapshot(roots, query, false), "local scoped query has no network snapshot hint despite configured roots");
        query.path_prefix.clear();
        check(pulse::app::UsesNetworkSnapshot(roots, query, false), "global query retains configured network snapshot hint");
        check(!pulse::app::UsesNetworkSnapshot({}, query, false), "global query without network roots has no snapshot hint");
        query.path_prefix = root.path;
        check(pulse::app::UsesNetworkSnapshot(roots, query, false), "indexed network root retains snapshot hint");
        query.path_prefix = root.path + L"\\child";
        check(pulse::app::UsesNetworkSnapshot(roots, query, false), "indexed network descendant retains snapshot hint");
        check(!pulse::app::UsesNetworkSnapshot(roots, query, true), "live network query leaves hint to existing live banner");
        query.path_prefix = root.path + L"-other";
        check(!pulse::app::UsesNetworkSnapshot(roots, query, false), "sibling prefix cannot borrow another root snapshot hint");
        query.path_prefix = L"\\\\FIXTURE.INVALID\\SHARE\\child";
        check(pulse::app::UsesNetworkSnapshot(roots, query, false), "scope comparison reuses case-insensitive production root helper");
        roots[0].building = true;
        check(!pulse::app::UsesNetworkSnapshot(roots, query, false), "building root does not promise snapshot coverage");
        roots[0].building = false; roots[0].online = false;
        check(!pulse::app::UsesNetworkSnapshot(roots, query, false), "offline scoped root does not promise snapshot coverage");
        std::cout << "Failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    auto normalize = [&](DWORD returned, DWORD last_error, DWORD bytes,
                         int calls, bool success, const char* name) {
        universal_first_result = returned;
        universal_last_error = last_error;
        universal_reported_bytes = bytes;
        universal_calls = 0;
        const auto normalized = NormalizeNetworkRoot(L"Z:\\synthetic");
        check(universal_calls == calls && normalized == (success ? root_path : L""), name);
    };
    const DWORD valid_bytes = static_cast<DWORD>(sizeof(UNIVERSAL_NAME_INFOW) +
                                                (root_path.size() + 1) * sizeof(wchar_t));
    normalize(ERROR_MORE_DATA, ERROR_SUCCESS, valid_bytes, 2, true,
              "mapped drive follows returned MORE_DATA with LastError zero");
    normalize(ERROR_MORE_DATA, ERROR_ACCESS_DENIED, valid_bytes, 2, true,
              "mapped drive follows returned MORE_DATA with unrelated LastError");
    normalize(ERROR_ACCESS_DENIED, ERROR_MORE_DATA, valid_bytes, 1, false,
              "mapped drive rejects failure despite stale LastError MORE_DATA");
    normalize(ERROR_MORE_DATA, ERROR_SUCCESS, 0, 1, false,
              "mapped drive rejects zero buffer size");
    normalize(ERROR_MORE_DATA, ERROR_SUCCESS, static_cast<DWORD>(sizeof(UNIVERSAL_NAME_INFOW) - 1),
              1, false, "mapped drive rejects undersized info buffer");
    normalize(ERROR_MORE_DATA, ERROR_SUCCESS,
              static_cast<DWORD>(sizeof(UNIVERSAL_NAME_INFOW) + 32768 * sizeof(wchar_t) + 1),
              1, false, "mapped drive rejects excessive buffer size");
    universal_calls = 0;
    check(NormalizeNetworkRoot(root_path) == root_path && universal_calls == 0,
          "direct UNC bypasses mapped drive lookup");
    auto live = [&](WalkCase scenario, DWORD error, bool complete, size_t count, const char* name) {
        walk_case = scenario;
        LiveNetworkMatches matches;
        std::mutex mutex;
        LiveNetworkWalk(root_path, L"", false, matches, mutex, [] { return false; }, {});
        check(matches.error == error && matches.complete == complete && matches.total == count &&
              handles_open == 0 && !unexpected_path, name);
    };
    live(WalkCase::Normal, ERROR_SUCCESS, true, 1, "live normal EOF survives FindClose last-error overwrite");
    live(WalkCase::Empty, ERROR_SUCCESS, true, 0, "live empty FindFirst completes");
    live(WalkCase::EmptyEof, ERROR_SUCCESS, true, 0, "live empty normal EOF completes");
    live(WalkCase::RootDenied, ERROR_ACCESS_DENIED, true, 0, "live root denial finishes with partial-result error");
    live(WalkCase::ChildDenied, ERROR_ACCESS_DENIED, true, 1, "live child denial finishes with partial-result error");
    live(WalkCase::Interrupted, ERROR_NETNAME_DELETED, true, 1, "live interrupted enumeration finishes with preserved error");
    {
        walk_case = WalkCase::Normal;
        LiveNetworkMatches matches;
        std::mutex mutex;
        LiveNetworkWalk(root_path, L"", false, matches, mutex, [] { return true; }, {});
        check(!matches.complete && matches.total == 0 && handles_open == 0, "live cancellation remains incomplete");
    }

    const auto temp = std::filesystem::temp_directory_path();
    const auto fixture = temp / (L"pulse-network-walk-test-" + std::to_wstring(GetCurrentProcessId()) +
                                 L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    if (!std::filesystem::create_directory(fixture, ec)) {
        check(false, "create isolated shard fixture");
        return 1;
    }
    fixture_directory = fixture.wstring();
    {
        NetworkIndex index;
        NetworkIndexTestAccess::Prepare(index);
        walk_case = WalkCase::Normal;
        NetworkIndexTestAccess::Build(index);
        const bool ready = NetworkIndexTestAccess::Ready(index);
        check(ready, "BuildRoot publishes initial isolated shard");
        if (ready) {
            const void* old = NetworkIndexTestAccess::ShardIdentity(index);
            const auto shard_path = ShardPath(root_path, 0);
            const auto before = ReadFixture(shard_path);
            check(!before.empty(), "initial shard bytes exist in fixture");
            for (WalkCase scenario : {WalkCase::RootDenied, WalkCase::ChildDenied, WalkCase::Interrupted}) {
                walk_case = scenario;
                NetworkIndexTestAccess::Build(index);
                check(NetworkIndexTestAccess::FailedAndRetained(index, old) &&
                      ReadFixture(shard_path) == before &&
                      !std::filesystem::exists(ShardPath(root_path, 1)) && handles_open == 0,
                      scenario == WalkCase::RootDenied ? "BuildRoot root failure preserves shard" :
                      scenario == WalkCase::ChildDenied ? "BuildRoot child failure preserves shard" :
                      "BuildRoot interrupted enumeration preserves shard");
            }
            check(NetworkIndexTestAccess::FailedCrawlRetriesWithWatch(index),
                  "failed crawl retries after five minutes with healthy watch");
            for (WalkCase scenario : {WalkCase::ChildDenied, WalkCase::Interrupted}) {
                walk_case = scenario;
                check(NetworkIndexTestAccess::FailedSubtreeKeepsOverlay(index),
                      scenario == WalkCase::ChildDenied ? "denied subtree discards partial overlay and schedules crawl" :
                      "interrupted subtree discards partial overlay and schedules crawl");
            }
        }
    }
    // GetTempPath leaves a trailing separator; path equality treats that as an
    // extra empty component. Compare directory identity before deleting our child.
    std::error_code guard_error;
    const bool same_parent = std::filesystem::equivalent(fixture.parent_path(), temp, guard_error);
    const auto fixture_status = std::filesystem::symlink_status(fixture, ec);
    const bool owned_directory = same_parent && !guard_error && !ec &&
        fixture_status.type() == std::filesystem::file_type::directory &&
        fixture.filename().wstring().starts_with(L"pulse-network-walk-test-");
    std::wcout << L"cleanup root=[" << fixture.wstring() << L"] temp=[" << temp.wstring()
               << L"] lexical_parent_match=" << (fixture.parent_path() == temp)
               << L" directory_parent_match=" << same_parent << L'\n';
    if (owned_directory) std::filesystem::remove_all(fixture, ec);
    std::error_code exists_error;
    const bool remains = std::filesystem::exists(fixture, exists_error);
    if (!owned_directory || ec || exists_error || remains) {
        std::cerr << "cleanup guard_error=" << guard_error.value() << " (" << guard_error.message()
                  << "), removal_error=" << ec.value() << " (" << ec.message()
                  << "), exists_error=" << exists_error.value() << " (" << exists_error.message()
                  << "), remains=" << remains << '\n';
    }
    check(owned_directory && !ec && !exists_error && !remains, "isolated shard fixture cleaned up");
    return failures == 0 ? 0 : 1;
}
