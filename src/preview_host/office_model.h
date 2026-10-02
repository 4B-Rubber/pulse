// office_model.h - first-page content of Word / Excel files for thumbnails.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {

enum class DocAlign { Left, Center, Right, Justify };

struct DocBlock {
    bool table = false;
    std::wstring text;                               // paragraph text (empty = blank line)
    float points = 10.5f;                            // font size in points
    bool bold = false;
    DocAlign align = DocAlign::Left;
    std::vector<std::vector<std::wstring>> rows;     // table: first rows, cell texts
};

// Page geometry of the first section (points) and its first blocks.
struct DocModel {
    float page_width = 595.3f, page_height = 841.9f;
    float margin_top = 72.0f, margin_left = 90.0f, margin_right = 90.0f;
    int columns = 1;
    float column_gap = 21.25f;
    std::vector<DocBlock> blocks;
};

// Top-left used range of the first worksheet.
struct SheetModel {
    int first_row = 1, first_col = 1;                // 1-based
    std::vector<std::vector<std::wstring>> grid;     // [row][col], blank = empty cell
};

// Reads word/document.xml (the first ~40 body paragraphs / tables, sizes
// resolved through styles.xml) and the first section's page setup. Only a
// prefix of large documents is read. False when the file is not a readable
// WordprocessingML package.
bool ReadDocxModel(const std::wstring& path, DocModel& model, std::wstring* error);

// Reads the first sheet (workbook order, resolved through its relationship),
// starting at the first non-empty cell: up to `rows` x `cols` cells. Shared and
// inline strings are resolved; numbers are shown as stored.
bool ReadXlsxModel(const std::wstring& path, int rows, int cols, SheetModel& model,
                   std::wstring* error);

} // namespace pulse::preview
