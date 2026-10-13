#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {
bool DecodeGifFrame(const std::wstring& path, DWORD attrs, UINT pixels,
    uint32_t frame_index, std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride,
    uint32_t& frame_count, uint32_t& delay_ms, uint32_t& loop_count,
    UINT& source_width, UINT& source_height, std::wstring* error = nullptr);
}
