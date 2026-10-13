#pragma once
#include "../fs/fs_snapshot.h"
#include <string_view>

namespace pulse::app {
struct ContentSearchSnapshot {
    fs::SnapshotPtr snapshot;
    bool retained = false;
};

inline ContentSearchSnapshot PrepareContentSearchSnapshot(fs::SnapshotPtr previous,
    std::wstring_view snapshot_path, std::wstring_view query_path, fs::SnapshotPtr empty_results) {
    const bool retain = previous && !previous->empty() && !query_path.empty() &&
        snapshot_path == query_path;
    return {retain ? std::move(previous) : std::move(empty_results), retain};
}
} // namespace pulse::app
