#pragma once

#include <string>
#include <vector>

namespace pulse::ui {

// One embedded release note (docs/releases/<version>.md), pre-split for painting.
struct ReleaseNoteView {
    std::wstring version;
    bool current = false;              // the version that is running
    std::vector<std::wstring> lines;   // plain text, Markdown markers stripped
    std::vector<bool> bullets;         // parallel to lines: list item vs. paragraph
};

} // namespace pulse::ui
