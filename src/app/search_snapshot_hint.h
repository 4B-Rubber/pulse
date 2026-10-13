#pragma once
#include "../index/network_index.h"

namespace pulse::app {
inline bool UsesNetworkSnapshot(const std::vector<index::NetworkRootInfo>& roots,
                                const index::Query& query, bool live_network) {
    if (live_network || roots.empty()) return false;
    return query.path_prefix.empty() || index::NetworkRootsCover(roots, query.path_prefix, true);
}
}
