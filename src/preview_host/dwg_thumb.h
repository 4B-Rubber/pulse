// dwg_thumb.h — preview image embedded in AutoCAD DWG files.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {

// AutoCAD stores a small preview picture (a BMP DIB, or PNG since R2013) in
// every R13+ drawing it saves. This reads it straight from the file header -
// no AutoCAD / DWG TrueView install, no shell handler - and returns 32bpp
// premultiplied BGRA scaled down to max_edge (never up; the UI scales).
// Returns false with *error set when the drawing has no preview (older than
// R13, saved without a thumbnail, or only a WMF preview) so the caller can
// fall back to the shell thumbnail.
bool ExtractDwgThumbnail(const std::wstring& path, UINT max_edge,
                         std::vector<unsigned char>& pixels,
                         UINT& width, UINT& height, UINT& stride,
                         UINT& source_width, UINT& source_height,
                         std::wstring* error);

} // namespace pulse::preview
