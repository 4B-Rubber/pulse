#pragma once
#include "app_model.h"

namespace pulse::app {
inline void LayoutWindowPanes(const SplitContainer& root, const D2D1_RECT_F& bounds,
                              float scale, std::vector<std::pair<Pane*, D2D1_RECT_F>>& out,
                              std::vector<SplitterLayout>* splitters = nullptr) {
    LayoutSplitTree(root, bounds, 8.0f * scale, out, splitters);
}

inline D2D1_RECT_F FocusedWindowPaneRect(const SplitContainer* root, Pane* pane,
                                       const D2D1_RECT_F& bounds, float scale) {
    if (!root || !pane) return bounds;
    std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
    // Input must use the rendered split gap: even a few pixels can change
    // the grid's column count at a wrapping boundary.
    LayoutWindowPanes(*root, bounds, scale, laid);
    for (const auto& item : laid) if (item.first == pane) return item.second;
    return laid.empty() ? bounds : laid.front().second;
}
}
