// network_crawl_schedule.h - When the per-user SMB agent re-crawls a root.
//
// A crawl walks the whole share and rewrites its shard, which on a large
// share takes many minutes of SMB traffic and hundreds of MB of writes. The
// agent used to crawl every root at start, after every change notification
// and every 5 minutes, so a share whose crawl outlasted the interval was
// crawled back to back forever. The rules here keep the index fresh through
// the directory watch and crawl only when that is not enough:
//  - explicit requests (new root, Rebuild, abandoned crawl): now;
//  - start-up with a shard on disk: once it is kReconcileMin old, but no
//    sooner than a short delay, so the previous session's index is used and
//    the watch covers the gap;
//  - observed changes: after kChangeQuiet without further changes (at most
//    kChangeMaxDelay after the first), and never closer to the previous crawl
//    than ChangeSpacing, which scales with how long a crawl takes;
//  - no working watch (server offline, watch failed): reconcile every
//    ReconcileInterval, or retry after kOfflineRetry when the last crawl
//    failed. A healthy watch reports overflow and disconnects itself, so it
//    needs no periodic crawl.
#pragma once
#include <algorithm>
#include <chrono>

namespace pulse::index::crawl_schedule {

using Clock = std::chrono::steady_clock;

constexpr auto kChangeQuiet = std::chrono::seconds(60);
constexpr auto kChangeMaxDelay = std::chrono::minutes(10);
constexpr auto kChangeMinSpacing = std::chrono::minutes(10);
constexpr int kChangeSpacingFactor = 4;
constexpr auto kReconcileMin = std::chrono::minutes(30);
constexpr int kReconcileFactor = 10;
constexpr auto kOfflineRetry = std::chrono::minutes(5);
constexpr auto kStartupMinDelay = std::chrono::seconds(60);

inline Clock::duration ChangeSpacing(Clock::duration last_crawl) {
    return (std::max)(Clock::duration(kChangeMinSpacing), last_crawl * kChangeSpacingFactor);
}

inline Clock::duration ReconcileInterval(Clock::duration last_crawl) {
    return (std::max)(Clock::duration(kReconcileMin), last_crawl * kReconcileFactor);
}

// Pending changes first/last observed at the given times. last_crawl_end is
// the epoch when the root has not been crawled in this session.
inline Clock::time_point ChangeDue(Clock::time_point first_change, Clock::time_point last_change,
                                   Clock::time_point last_crawl_end, Clock::duration last_crawl) {
    Clock::time_point due = (std::min)(last_change + kChangeQuiet, first_change + kChangeMaxDelay);
    if (last_crawl_end != Clock::time_point{})
        due = (std::max)(due, last_crawl_end + ChangeSpacing(last_crawl));
    return due;
}

// First crawl after the agent starts with a shard of the given age on disk.
inline Clock::time_point StartupDue(Clock::time_point now, Clock::duration shard_age) {
    const Clock::duration until_stale = Clock::duration(kReconcileMin) - shard_age;
    return now + (std::max)(Clock::duration(kStartupMinDelay), until_stale);
}

// Periodic reconcile for a root without a working watch.
inline Clock::time_point UnwatchedDue(Clock::time_point last_crawl_end, Clock::duration last_crawl,
                                      bool last_crawl_failed) {
    return last_crawl_end + (last_crawl_failed ? Clock::duration(kOfflineRetry)
                                               : ReconcileInterval(last_crawl));
}

} // namespace pulse::index::crawl_schedule
