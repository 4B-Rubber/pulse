#pragma once

#include "index_engine.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace pulse::index {

struct NetworkRootInfo {
    std::wstring path;
    bool online = false;
    bool building = false;
    bool watching = false;
    uint64_t indexed_items = 0;
    uint32_t progress = 0;
    std::wstring state;
    std::wstring error;
};

std::wstring NormalizeNetworkRoot(std::wstring path);
std::wstring NetworkConfigPath();
bool LoadNetworkRootsFile(const std::wstring& path, std::vector<std::wstring>& roots,
                          std::wstring* error = nullptr);
bool SaveNetworkRootsFile(const std::wstring& path, const std::vector<std::wstring>& roots,
                          std::wstring* error = nullptr);
bool LoadNetworkRoots(std::vector<std::wstring>& roots, std::wstring* error = nullptr);
bool SaveNetworkRoots(const std::vector<std::wstring>& roots, std::wstring* error = nullptr);

// Per-user SMB index. It intentionally runs in Pulse.Index.exe's
// --network-agent mode so Windows can use the interactive user's existing SMB
// session without the SYSTEM service storing credentials.
class NetworkIndex {
public:
    NetworkIndex() = default;
    ~NetworkIndex() { Stop(); }
    NetworkIndex(const NetworkIndex&) = delete;
    NetworkIndex& operator=(const NetworkIndex&) = delete;

    void Start(HWND notify, UINT status_msg, UINT search_msg);
    void Stop();
    void SearchAsync(const Query& query, uint32_t id);
    bool TakeResult(uint32_t id, SearchResult& result);

    std::vector<NetworkRootInfo> Roots() const;
    std::wstring ConfigError() const;
    bool AddRoot(const std::wstring& path, std::wstring* error = nullptr);
    bool RemoveRoot(const std::wstring& path, std::wstring* error = nullptr);
    void Rebuild(const std::wstring& path = {});
    ChangeTracker& Changes() { return changes_; }
    void SetChangeLease(const std::wstring& owner, bool enabled);
    ChangeState ChangeCoverage(const std::wstring& path) const;

private:
    friend struct NetworkIndexTestAccess;
    bool ReadConfig(const std::wstring& path, std::vector<std::wstring>& configured);
    ChangeTracker changes_;
    std::mutex change_seed_mutex_;
    std::unordered_set<std::wstring> change_seed_owners_;
    void SeedChanges(const std::wstring& owner);
    void SeedPendingChanges();
    void ObserveChanges(const std::wstring& root, const BYTE* data, DWORD bytes);
    void NoteRootChanged(const std::wstring& root);
    struct Shard;
    // Changes seen by the directory watch since the shard was built; lets
    // search stay current without re-crawling the share.
    struct Overlay;
    struct OverlayView;
    struct RootState {
        NetworkRootInfo info;
        std::shared_ptr<Shard> shard;
        std::shared_ptr<Overlay> overlay;
        // Crawl scheduling (network_crawl_schedule.h); epoch = not set.
        std::chrono::steady_clock::time_point startup_due{};
        std::chrono::steady_clock::time_point last_crawl_end{};
        std::chrono::steady_clock::duration last_crawl{};
        bool last_crawl_failed = false;
        bool change_pending = false;
        std::chrono::steady_clock::time_point change_first{};
        std::chrono::steady_clock::time_point change_last{};
    };
    // Roots whose crawl is due now go to `due`; returns the next future due
    // time (time_point::max() when nothing is scheduled). Caller holds mu_.
    std::chrono::steady_clock::time_point CollectDueRootsLocked(
        std::chrono::steady_clock::time_point now, std::vector<std::wstring>& due);

    // Immutable snapshot of root.overlay for the search thread (null when
    // empty). Caller holds mu_.
    std::shared_ptr<const OverlayView> OverlayViewLocked(RootState& root);
    // Lists directories that appeared (created or renamed) since the shard
    // was built; a directory watch reports only the directory itself.
    void ScanPendingSubtrees();

    void CrawlLoop();
    void WatchLoop();
    void SetWatching(const std::wstring& path, bool watching);
    bool ReloadConfigurationLocked(std::wstring* error);
    void SearchLoop();
    void BuildRoot(const std::wstring& path, uint64_t generation);
    void NotifyStatus() const;
    bool RootStillCurrent(const std::wstring& path, uint64_t generation) const;

    HWND notify_ = nullptr;
    UINT status_msg_ = 0;
    UINT search_msg_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> latest_search_id_{0};
    mutable std::mutex mu_;
    std::condition_variable crawl_cv_;
    std::condition_variable search_cv_;
    std::vector<RootState> roots_;
    std::wstring config_error_;
    std::unordered_set<std::wstring> dirty_roots_;
    uint64_t generation_ = 1;
    Query pending_query_;
    uint32_t pending_id_ = 0;
    bool have_pending_search_ = false;
    uint32_t result_id_ = 0;
    SearchResult result_;
    std::thread crawl_thread_;
    std::thread watch_thread_;
    std::thread search_thread_;
    HANDLE watch_wake_event_ = nullptr;
};

SearchResult MergeSearchResults(const Query& query, SearchResult local,
                                SearchResult network, bool deduplicate_paths = false);

// #74: name search in a network folder that no ready network root covers. The
// local index never holds SMB paths and the network index only answers for
// roots added in Settings, so such a scoped search found nothing while
// Explorer walks the folder. These walk it live with the network index's
// matching and ordering rules.
struct LiveNetworkMatches {
    std::vector<Hit> hits;  // bounded top-k for order, or traversal order for Index
    Query order;
    size_t total = 0;       // all matches, including past the cap
    DWORD error = 0;        // any directory or enumeration failed
    bool finished = false;
    bool complete = false;  // walk finished; nonzero error means partial results
};
// UNC paths and mapped network drives.
bool IsNetworkFolderPath(const std::wstring& path);
// True when `path` lies under a configured root; with require_ready only roots
// whose index can answer a search right now count.
bool NetworkRootsCover(const std::vector<NetworkRootInfo>& roots, const std::wstring& path,
                       bool require_ready);
// Replaces out with a bounded selection while recursively listing the scope.
// The Query overload retains its ordered top-k; the string overload keeps
// traversal order for compatibility. Progress runs about every 250 ms;
// cancellation is checked between enumeration entries.
void LiveNetworkWalk(const std::wstring& folder, const std::wstring& needle, bool folders_only,
                     LiveNetworkMatches& out, std::mutex& out_mutex,
                     const std::function<bool()>& cancelled,
                     const std::function<void()>& progress);
void LiveNetworkWalk(const Query& query, LiveNetworkMatches& out, std::mutex& out_mutex,
                     const std::function<bool()>& cancelled,
                     const std::function<void()>& progress);
// The complete capped prefix is retained, so pagination can reuse a walk;
// changes to matching, ordering or session require a fresh walk.
bool CanReuseLiveNetworkWalk(const Query& previous, const Query& next);
// One provider answer (offset/limit applied) from the matches so far. The
// caller holds the mutex guarding `matches`.
SearchResult SelectLiveNetworkHits(const Query& query, const LiveNetworkMatches& matches);

} // namespace pulse::index

namespace pulse::index {
void LiveNetworkWalk(const std::wstring& folder, const std::wstring& needle, bool folders_only,
    LiveNetworkMatches& out, std::mutex& mutex, const std::function<bool()>& cancelled,
    const std::function<void()>& progress, const Query* selection);
}
