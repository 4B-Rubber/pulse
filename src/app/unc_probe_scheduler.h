#pragma once

#include <cstdint>

namespace pulse::app {

// Tracks ownership of the single UI-side UNC probe slot. A result from an
// expired probe must never release a slot acquired by a later probe.
struct UncProbeScheduler {
    uint64_t next_id = 1;
    uint64_t active_id = 0;

    uint64_t Begin() {
        uint64_t id = next_id++;
        if (id == 0) id = next_id++;
        active_id = id;
        return id;
    }

    bool IsActive(uint64_t id) const {
        return id != 0 && id == active_id;
    }

    bool Finish(uint64_t id) {
        if (!IsActive(id)) return false;
        active_id = 0;
        return true;
    }
};

} // namespace pulse::app
