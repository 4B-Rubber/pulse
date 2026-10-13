#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace pulse::app {

// Tracks ownership of the single UI-side UNC probe slot. A result from an
// expired probe must never release a slot acquired by a later probe.
struct UncProbeScheduler {
    uint64_t next_id = 1;
    uint64_t active_id = 0;

    uint64_t Begin(const std::wstring& path = {}) {
        uint64_t id = next_id++;
        if (id == 0) id = next_id++;
        active_id = id;
        latest_by_path_[path] = id;
        return id;
    }

    // Slot ownership and observation freshness are separate. A timeout frees
    // the slot but the same request may still report recovery for this path.
    bool Accept(const std::wstring& path, uint64_t id, bool completed) {
        return AcceptResult(path, id, completed);
    }

    bool IsActive(uint64_t id) const {
        return id != 0 && id == active_id;
    }

    bool Finish(uint64_t id) {
        if (!IsActive(id)) return false;
        active_id = 0;
        return true;
    }

    // Slot ownership and result freshness are separate: after a timeout the
    // same path's final result is useful until a newer probe supersedes it.
    bool AcceptResult(const std::wstring& path, uint64_t id, bool final) {
        const auto it = latest_by_path_.find(path);
        if (id == 0 || it == latest_by_path_.end() || it->second != id) return false;
        if (final) latest_by_path_.erase(it);
        return true;
    }

    void Cancel(const std::wstring& path, uint64_t id) {
        if (AcceptResult(path, id, true)) Finish(id);
    }

    void Clear() {
        latest_by_path_.clear();
        active_id = 0;
        // Do not reuse IDs if this owner is reused after shutdown/reset.
    }

private:
    std::unordered_map<std::wstring, uint64_t> latest_by_path_;
};

} // namespace pulse::app
