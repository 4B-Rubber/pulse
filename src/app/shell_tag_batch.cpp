#include "shell_tag_batch.h"

namespace pulse::app::shell_tags {
Reply BatchHandler::Handle(PlacesCatalog& places, const Request& request, const AdsSink& ads, uint64_t now,
    bool* added, bool* duplicate) {
    if (added) *added = false;
    if (duplicate) *duplicate = false;
    const TagId id = places.ResolveTagRef(request.tag);
    if (id.empty() || !places.FindTag(id) || places.load_failed) return Reply::Failed;
    Reply prior = Reply::Failed;
    const auto accepted = receipts_.Accept(request, now, prior);
    if (accepted == Receipts::Acceptance::Duplicate) { if (duplicate) *duplicate = true; return prior; }
    if (accepted == Receipts::Acceptance::Invalid) return Reply::Failed;
    const bool add = places.GetSelectionState(id, request.paths) != TagSelectionState::All;
    if (added) *added = add;
    std::vector<TagAdsUpdate> updates;
    if (!places.SetTagsBatch(id, request.paths, add, &updates)) return Reply::Failed;
    // Record the completed mutation before notifying a sink which may pump messages.
    receipts_.Complete(request, Reply::Applied);
    if (ads) ads(std::move(updates));
    return Reply::Applied;
}
BatchHandler& ProcessBatchHandler() {
    static BatchHandler handler;
    return handler;
}
}
