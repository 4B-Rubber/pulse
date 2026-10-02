// font_raster.h — font file specimens for the preview host.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {

// Renders a specimen page for a TrueType / OpenType font file (.ttf .otf
// .ttc .otc) without installing it: family name, format and glyph count, the
// alphabet and digits, a CJK sample when the font covers it, and a size
// waterfall. Output is 32bpp premultiplied BGRA on a white page whose longest
// edge is max_edge. DirectWrite and Direct2D are loaded on demand.
bool RasterizeFontFile(const std::wstring& path, UINT max_edge,
                       std::vector<unsigned char>& pixels, UINT& width, UINT& height,
                       UINT& stride, std::wstring* error);

} // namespace pulse::preview
