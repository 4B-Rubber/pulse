#pragma once
#include "folder_sizes.h"
#include <windows.h>
#include <atomic>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace pulse::app::folder_size {
struct CompletedSubtree {
    std::wstring path;
    FolderSizeValue value;
    uint64_t watch_generation = 0;
};
class Scan {
public:
    using Lookup = std::function<std::optional<uint64_t>(const std::wstring&)>;
    using Coverage = std::function<uint64_t(const std::wstring&)>;
    Scan(std::wstring path, uint64_t revision, uint64_t request_epoch, bool manual = false);
    ~Scan();
    void Step(const std::atomic<bool>& stopping, const Lookup& lookup, const Coverage& coverage);
    FolderSizeValue Progress() const;
    uint64_t progress_reported_at = 0;
    std::wstring path;
    uint64_t revision = 0, request_epoch = 0;
    uint64_t entries_scanned = 0, subtree_hits = 0, active_ms = 0;
    FolderSizeValue result{FolderSizeState::Calculating};
    std::deque<CompletedSubtree> completed;
    bool done = false;
private:
    struct Frame {
        std::wstring path;
        HANDLE find = INVALID_HANDLE_VALUE;
        WIN32_FIND_DATAW data{};
        uint64_t bytes = 0, watch_generation = 0, skipped = 0;
        uint32_t issues = 0;
        bool started = false, partial = false, read_any = false;
    };
    bool manual_ = false;
    std::vector<Frame> stack_;
    void FinishFrame();
};
} // namespace pulse::app::folder_size