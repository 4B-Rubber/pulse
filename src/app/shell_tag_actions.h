#pragma once
#include "shell_tag_menu.h"
#include "places.h"
#include <deque>
#include <set>
#include <utility>
#include <vector>

namespace pulse::app {
struct ShellTagBatch {
    std::wstring tag_id;
    ShellTagAction action = ShellTagAction::Add;
    std::vector<std::wstring> paths;
};
inline std::vector<ShellTagBatch> GroupShellTagActions(const std::vector<ShellTagRequest>& requests) {
    std::vector<ShellTagBatch> batches;
    for (const auto& request : requests) {
        if (batches.empty() || batches.back().tag_id != request.tag_id || batches.back().action != request.action)
            batches.push_back({request.tag_id, request.action, {}});
        batches.back().paths.push_back(request.path);
    }
    return batches;
}
inline bool ApplyShellTagBatch(PlacesCatalog& places, const ShellTagBatch& request,
    std::vector<TagAdsUpdate>& updates, std::wstring& name) {
    const auto id = places.ResolveTagRef(request.tag_id);
    const auto* tag = id.empty() ? nullptr : places.FindTag(id);
    if (!tag) return false;
    name = tag->name;
    return places.SetTagsBatch(id, request.paths, request.action == ShellTagAction::Add, &updates);
}
// Explorer's individual launches are explicit assignments, never fragments of
// a guessed toggle batch. Retried IDs remain suppressed across later actions.
class ShellTagActions {
public:
    bool Push(ShellTagRequest request) {
        if (request.tag_id.empty() || request.path.empty()) return false;
        if (!request.operation_id.empty()) {
            if (!seen_.insert(request.operation_id).second) return false;
            accepted_.push_back(request.operation_id);
            if (accepted_.size() > 8192) {
                seen_.erase(accepted_.front());
                accepted_.pop_front();
            }
        }
        pending_.push_back(std::move(request));
        return true;
    }
    std::vector<ShellTagRequest> Drain() { return std::exchange(pending_, {}); }
    bool empty() const { return pending_.empty(); }
private:
    std::vector<ShellTagRequest> pending_;
    std::deque<std::wstring> accepted_;
    std::set<std::wstring> seen_;
};
} // namespace pulse::app
