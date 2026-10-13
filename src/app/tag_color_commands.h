#pragma once
#include <cstddef>
#include <limits>

namespace pulse::app {
inline constexpr int kCreateTagCommand = 29999;
inline constexpr int kTagColorStripCommand = 30050;
inline constexpr int kCustomTagColorCommand = 30051;
inline constexpr int kTagColorBaseCommand = 30060;
inline constexpr size_t kMaxTagColorCommands = static_cast<size_t>((std::numeric_limits<int>::max)() - kTagColorBaseCommand);
inline bool IsTagColorCommand(int command, size_t palette_size) {
    return command >= kTagColorBaseCommand &&
        static_cast<size_t>(command - kTagColorBaseCommand) < palette_size;
}
}
