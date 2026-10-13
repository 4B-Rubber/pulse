#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::index {
inline HRESULT AppendPdfUnicode(std::wstring& text, uint32_t value, size_t capacity) {
    // PDFium uses zero for characters without a Unicode mapping.
    if (!value) return S_OK;
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    const size_t units = value > 0xffff ? 2 : 1;
    if (text.size() > capacity || units > capacity - text.size())
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    if (units == 1) text += static_cast<wchar_t>(value);
    else {
        value -= 0x10000;
        text += static_cast<wchar_t>(0xd800 + (value >> 10));
        text += static_cast<wchar_t>(0xdc00 + (value & 0x3ff));
    }
    return S_OK;
}
} // namespace pulse::index
