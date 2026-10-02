#pragma once
#include <algorithm>
#include <cstdint>

namespace pulse::app {
struct PaneHeaderAnimation {
    float opacity = 0.0f;
    float from = 0.0f;
    float target = 0.0f;
    uint64_t started = 0;
    bool initialized = false;

    bool Tick(bool focused, uint64_t now) noexcept {
        const float before = opacity;
        const float next = focused ? 1.0f : 0.0f;
        if (!initialized) {
            initialized = true;
            opacity = from = target = next;
        } else if (next != target) {
            from = opacity;
            target = next;
            started = now;
        }
        if (opacity != target) {
            const float t = std::clamp(static_cast<float>(now - started) / 180.0f, 0.0f, 1.0f);
            const float remaining = 1.0f - t;
            opacity = t >= 1.0f ? target : from + (target - from) *
                (1.0f - remaining * remaining * remaining);
        }
        return before != opacity;
    }
};
}
