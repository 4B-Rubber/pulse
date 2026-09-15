#pragma once
#include "content_index.h"
#include "content_timing.h"
#include <optional>

namespace pulse::index {
struct ContentTaskCache {
    std::function<std::wstring(const std::wstring &)> version;
    std::function<bool(const std::wstring &)> excluded;
    std::function<bool(const std::wstring &, uint64_t, uint64_t, const std::wstring &)> fresh;
    bool native_version = false;
};
using ContentTaskReader = std::function<bool(const std::wstring &, uint64_t, std::wstring &, uint64_t &, DWORD *,
                                             text::Encoding, const std::function<bool()> &)>;
// Callbacks run only on the calling thread. No content is retained after return.
bool RunContentTaskSupplement(const ContentIndexConfig &, const ContentSearchRequest &, const std::atomic<bool> &,
                              const ContentTaskCache &, ContentSearchProgress, size_t initial_hits,
                              ContentBatchCallback, ContentTaskReader reader = {}, ContentTiming* timing = nullptr);
} // namespace pulse::index
