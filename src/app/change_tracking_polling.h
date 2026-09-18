#pragma once
#include <windows.h>

namespace pulse::app {
inline bool ShouldPollChangeTracking(HWND window) {
    return window && IsWindowVisible(window) && !IsIconic(window);
}
}
