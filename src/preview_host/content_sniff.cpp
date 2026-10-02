// content_sniff.cpp - see content_sniff.h.
#include "content_sniff.h"
#include <windows.h>
#include <cstring>

namespace pulse::preview {

namespace {
bool StartsWith(const uint8_t* data, size_t size, const char* magic, size_t length) {
    return size >= length && std::memcmp(data, magic, length) == 0;
}
bool IsDigit(uint8_t c) { return c >= '0' && c <= '9'; }
} // namespace

SniffedFormat SniffContentBytes(const uint8_t* data, size_t size) {
    if (!data || size < 4) return SniffedFormat::None;
    // DWG: "AC1012" (R13) ... "AC1032" (2018+); ezdxf / LibreDWG read the same
    // version string. R12 and older ("AC1009") carry no header preview.
    if (StartsWith(data, size, "AC10", 4) && size >= 6 && IsDigit(data[4]) && IsDigit(data[5]) &&
        (data[4] > '1' || (data[4] == '1' && data[5] >= '2')))
        return SniffedFormat::Dwg;
    // TrueType / OpenType / collections.
    if (StartsWith(data, size, "\x00\x01\x00\x00", 4) || StartsWith(data, size, "OTTO", 4) ||
        StartsWith(data, size, "true", 4) || StartsWith(data, size, "ttcf", 4))
        return SniffedFormat::Font;
    if (StartsWith(data, size, "{\\rtf", 5)) return SniffedFormat::Rtf;
    if (StartsWith(data, size, "%PDF-", 5)) return SniffedFormat::Pdf;
    // Raster images WIC decodes by content whatever the file is called.
    if (StartsWith(data, size, "\x89PNG\r\n\x1A\n", 8) || StartsWith(data, size, "\xFF\xD8\xFF", 3) ||
        StartsWith(data, size, "GIF87a", 6) || StartsWith(data, size, "GIF89a", 6) ||
        StartsWith(data, size, "II*\x00", 4) || StartsWith(data, size, "MM\x00*", 4) ||
        (StartsWith(data, size, "RIFF", 4) && size >= 12 && std::memcmp(data + 8, "WEBP", 4) == 0))
        return SniffedFormat::Image;
    // BMP: "BM" alone is too common in text; also require the 40/108/124-byte
    // BITMAPINFOHEADER size at offset 14.
    if (StartsWith(data, size, "BM", 2) && size >= 18) {
        const uint32_t header = static_cast<uint32_t>(data[14]) | (static_cast<uint32_t>(data[15]) << 8) |
                                (static_cast<uint32_t>(data[16]) << 16) | (static_cast<uint32_t>(data[17]) << 24);
        if (header == 12 || header == 40 || header == 108 || header == 124) return SniffedFormat::Image;
    }
    // Archives libarchive lists, whatever the extension (.vsix / .nupkg /
    // .jar / .apk are ZIP): ZIP, 7-Zip, RAR 4/5, gzip, xz, CAB.
    if (StartsWith(data, size, "PK\x03\x04", 4) || StartsWith(data, size, "PK\x05\x06", 4) ||
        StartsWith(data, size, "7z\xBC\xAF\x27\x1C", 6) || StartsWith(data, size, "Rar!\x1A\x07", 6) ||
        StartsWith(data, size, "\x1F\x8B\x08", 3) || StartsWith(data, size, "\xFD" "7zXZ\x00", 6) ||
        StartsWith(data, size, "MSCF\x00\x00\x00\x00", 8))
        return SniffedFormat::Archive;
    return SniffedFormat::None;
}

SniffedFormat SniffContent(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return SniffedFormat::None;
    uint8_t head[32]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, head, sizeof(head), &read, nullptr);
    CloseHandle(file);
    return ok ? SniffContentBytes(head, read) : SniffedFormat::None;
}

} // namespace pulse::preview
