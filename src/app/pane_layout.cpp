#include "pane_layout.h"
#include <algorithm>
namespace pulse::app {
float ClampSplitRatio(float ratio, const D2D1_RECT_F& bounds, SplitOrientation orientation,
                      float gap) {
    const float g = std::max(0.0f, gap);
    const float span = (orientation == SplitOrientation::Vertical)
        ? std::max(0.0f, bounds.right - bounds.left - g)
        : std::max(0.0f, bounds.bottom - bounds.top - g);
    const float minPx = std::min(std::max(80.0f, g * 20.0f), span * 0.35f);
    float lo = span > 1.0f ? std::clamp(minPx / span, 0.08f, 0.45f) : 0.12f;
    float hi = 1.0f - lo;
    if (lo >= hi) {
        lo = 0.12f;
        hi = 0.88f;
    }
    return std::clamp(ratio, lo, hi);
}

void LayoutSplitTree(const SplitContainer& node, const D2D1_RECT_F& bounds, float gap,
                     std::vector<std::pair<Pane*, D2D1_RECT_F>>& out,
                     std::vector<SplitterLayout>* splitters) {
    if (node.is_leaf) {
        if (node.pane) out.push_back({node.pane, bounds});
        return;
    }
    const float g = std::max(0.0f, gap);
    const float span = (node.orientation == SplitOrientation::Vertical)
        ? std::max(0.0f, bounds.right - bounds.left - g)
        : std::max(0.0f, bounds.bottom - bounds.top - g);
    const float ratio = ClampSplitRatio(node.ratio, bounds, node.orientation, gap);
    D2D1_RECT_F a = bounds;
    D2D1_RECT_F b = bounds;
    D2D1_RECT_F hit = bounds;
    if (node.orientation == SplitOrientation::Vertical) {
        const float mid = bounds.left + span * ratio;
        a.right = mid;
        b.left = mid + g;
        const float hitHalf = std::max(g, 8.0f) * 0.5f;
        const float center = mid + g * 0.5f;
        hit.left = center - hitHalf;
        hit.right = center + hitHalf;
    } else {
        const float mid = bounds.top + span * ratio;
        a.bottom = mid;
        b.top = mid + g;
        const float hitHalf = std::max(g, 8.0f) * 0.5f;
        const float center = mid + g * 0.5f;
        hit.top = center - hitHalf;
        hit.bottom = center + hitHalf;
    }
    if (splitters) {
        SplitterLayout slot;
        slot.node = const_cast<SplitContainer*>(&node);
        slot.hit_rect = hit;
        slot.parent_bounds = bounds;
        slot.orientation = node.orientation;
        splitters->push_back(slot);
    }
    if (node.first) LayoutSplitTree(*node.first, a, gap, out, splitters);
    if (node.second) LayoutSplitTree(*node.second, b, gap, out, splitters);
}

}
