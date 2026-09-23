#pragma once
// Column edges are stored as per-ten-thousand integers ("1234,2345,6789"), the shape
// session.json and the per-folder view store both use, so both read and write them here. A zero
// in the first slot means "no stored edges": the defaults are computed from the window instead.
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace pulse {

inline std::wstring FormatScaled3(const std::array<float, 3>& edges) {
    return std::to_wstring(static_cast<int>(std::lround(edges[0] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[1] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[2] * 10000.0f)));
}

inline std::array<float, 3> ParseScaled3(const std::wstring& value) {
    std::array<int, 3> edges{};
    std::array<float, 3> ratios{};
    if (swscanf_s(value.c_str(), L"%d,%d,%d",
                  &edges[0], &edges[1], &edges[2]) == 3 &&
        edges[0] > 0 && edges[0] < edges[1] &&
        edges[1] < edges[2] && edges[2] < 10000) {
        for (size_t i = 0; i < ratios.size(); ++i)
            ratios[i] = static_cast<float>(edges[i]) / 10000.0f;
    }
    return ratios;
}

inline std::wstring FormatScaled4(const std::array<float, 4>& edges) {
    return std::to_wstring(static_cast<int>(std::lround(edges[0] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[1] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[2] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[3] * 10000.0f)));
}

inline std::array<float, 4> ParseScaled4(const std::wstring& value) {
    std::array<int, 4> edges{};
    std::array<float, 4> ratios{};
    if (swscanf_s(value.c_str(), L"%d,%d,%d,%d",
                  &edges[0], &edges[1], &edges[2], &edges[3]) == 4 &&
        edges[0] > 0 && edges[0] < edges[1] && edges[1] < edges[2] &&
        edges[2] < edges[3] && edges[3] < 10000) {
        for (size_t i = 0; i < ratios.size(); ++i)
            ratios[i] = static_cast<float>(edges[i]) / 10000.0f;
    }
    return ratios;
}

} // namespace pulse
