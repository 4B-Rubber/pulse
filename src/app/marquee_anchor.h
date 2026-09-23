// marquee_anchor.h — How the rubber band keeps its place while the list scrolls under it.
#pragma once

#include <cmath>

namespace pulse::app {

// Whole pixels to move the band's corners for a content movement of `moved`; the fraction the
// integer corners cannot hold stays in `residual`.
//
// The band hangs off the rows, so it has to travel exactly as far as they do. Rounding every
// frame on its own (the first version of this) dropped that fraction each time: at the end of
// a wheel notch the rows kept creeping while the band stood still, then jumped a pixel, and
// over a long scroll the rounding error piled up until the band no longer sat over the rows it
// had picked. Carrying the remainder keeps the two locked together.
inline int CarryMarqueeShift(float& residual, float moved) {
    residual += moved;
    const int steps = static_cast<int>(std::lround(residual));
    if (steps != 0) residual -= static_cast<float>(steps);
    return steps;
}

} // namespace pulse::app
