#pragma once
#include "../fs/fs_watch.h"
#include <algorithm>
#include <utility>

namespace pulse::app {

// Caller holds the notification queue mutex. A pending full reconciliation
// supersedes per-entry patches for the same path, but not other paths.
template<class Batch>
void QueueDirNotify(std::vector<Batch>& queue, const std::wstring& path,
                    bool overflow, std::vector<fs::DirNotifyEvent> events) {
    if (std::any_of(queue.begin(), queue.end(), [&](const auto& batch) {
        return batch.path == path && batch.overflow;
    })) return;
    if (overflow) {
        std::erase_if(queue, [&](const auto& batch) { return batch.path == path; });
        events.clear();
    }
    queue.push_back({path, overflow, std::move(events)});
}

} // namespace pulse::app
