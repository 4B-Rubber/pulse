// office_slide_model.h - first slide of a presentation, for grid thumbnails.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {

enum class SlideTextKind { Title, CenterTitle, Subtitle, Body, Other };

// Rectangles are fractions of the slide (0..1); x < 0 means the shape takes
// its place from the layout, which is not read: the sketch uses the default
// place for its kind.
struct SlideRect {
    double x = -1, y = 0, w = 0, h = 0;
};

struct SlideText {
    SlideTextKind kind = SlideTextKind::Other;
    SlideRect rect;
    double points = 0;                    // first run's size; 0 = default for the kind
    std::vector<std::wstring> paragraphs; // empty paragraphs kept as blank lines
};

struct SlideModel {
    double width_pt = 720, height_pt = 540;   // slide size (default 4:3 when absent)
    int slide_count = 0;
    std::vector<SlideText> texts;
    std::vector<SlideRect> pictures;          // pictures, charts, tables: drawn as boxes
};

// PowerPoint 2007+ (.pptx/.pptm/.ppsx, ZIP): presentation.xml for the slide
// size and order, then the first slide's shapes, placeholders and runs.
bool ReadPptxSlideModel(const std::wstring& path, SlideModel& model, std::wstring* error);

// PowerPoint 97-2003 and WPS presentation (.ppt/.pps/.dps, [MS-PPT] inside a
// compound file): the "PowerPoint Document" record tree - DocumentAtom for
// the size, the first SlideContainer's text boxes with their anchors, or the
// SlideListWithText text when the slide keeps none (Apache POI HSLF order).
bool ReadPptSlideModel(const std::wstring& path, SlideModel& model, std::wstring* error);

// Either, by content (ZIP or compound file), not by extension.
bool ReadSlideModel(const std::wstring& path, SlideModel& model, std::wstring* error);

} // namespace pulse::preview
