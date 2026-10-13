#pragma once
#include "shell_tag_protocol.h"
#include "places.h"
#include <functional>

namespace pulse::app::shell_tags {
class BatchHandler {
public:
    using AdsSink = std::function<void(std::vector<TagAdsUpdate>)>;
    Reply Handle(PlacesCatalog& places, const Request& request, const AdsSink& ads, uint64_t now,
        bool* added = nullptr, bool* duplicate = nullptr);
private:
    Receipts receipts_;
};
// One authority owns all tag changes and completed-request receipts.
BatchHandler& ProcessBatchHandler();
}
