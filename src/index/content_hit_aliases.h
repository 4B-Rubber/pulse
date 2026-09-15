#pragma once
#include "content_index.h"
#include <set>

namespace pulse::index {
// Owned by the task's callback thread. Source hits and aliases share one path
// identity set across cached results and the supplemental scan.
class ContentHitAliases {
public:
    ContentHitAliases(const ContentIndexConfig&, const ContentSearchRequest&,
        std::function<bool()> cancelled, std::function<bool(const std::wstring&)> excluded);
    bool Emit(const ContentHit&, const std::function<bool(ContentHit)>&);
private:
    bool Allowed(const std::wstring& path, const ContentHit& source) const;
    const ContentIndexConfig& config_;
    const ContentSearchRequest& request_;
    std::function<bool()> cancelled_;
    std::function<bool(const std::wstring&)> excluded_;
    CompiledQuery filename_;
    std::set<std::wstring> emitted_;
};
}
