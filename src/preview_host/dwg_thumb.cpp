// dwg_thumb.cpp — see dwg_thumb.h.
//
// Layout (Open Design Alliance DWG specification, "Image data"):
//   file header  0x00  "AC10xx" version string
//                0x0D  int32 absolute offset of the image data block
//   image block        16-byte start sentinel
//                      int32 overall size, uint8 entry count
//                      entries: uint8 code, int32 start, int32 size
//                      code 1 = header data, 2 = BMP (DIB without the
//                      BITMAPFILEHEADER), 3 = WMF, 6 = PNG
#include "dwg_thumb.h"
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace pulse::preview {
namespace {

constexpr unsigned char kImageSentinel[16] = {
    0x1F, 0x25, 0x6D, 0x07, 0xD4, 0x36, 0x28, 0x28,
    0x9D, 0x57, 0xCA, 0x3F, 0x9D, 0x44, 0x10, 0x2B};
constexpr uint32_t kMaxImageBytes = 16u * 1024 * 1024;

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

uint32_t U32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

bool ReadAt(HANDLE file, ULONGLONG offset, void* data, DWORD bytes) {
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(file, data, bytes, &got, nullptr) && got == bytes;
}

bool DecodeWithWic(const std::vector<unsigned char>& encoded, UINT max_edge,
                   std::vector<unsigned char>& pixels, UINT& width, UINT& height,
                   UINT& stride, UINT& source_width, UINT& source_height) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(encoded.data(), static_cast<UINT>(encoded.size())));
    if (!stream) return false;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT fw = 0, fh = 0;
    if (FAILED(frame->GetSize(&fw, &fh)) || !fw || !fh || fw > 8192 || fh > 8192) return false;
    source_width = fw;
    source_height = fh;

    ComPtr<IWICBitmapSource> source = frame;
    const UINT edge = max_edge ? max_edge : 256u;
    if ((std::max)(fw, fh) > edge) {
        const double k = static_cast<double>(edge) / (std::max)(fw, fh);
        const UINT tw = (std::max)(1u, static_cast<UINT>(fw * k + 0.5));
        const UINT th = (std::max)(1u, static_cast<UINT>(fh * k + 0.5));
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant)))
            source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || !w || !h) return false;
    std::vector<unsigned char> out(static_cast<size_t>(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size()), out.data())))
        return false;
    pixels.swap(out);
    width = w;
    height = h;
    stride = w * 4;
    return true;
}

// The BMP entry is a packed DIB; WIC wants a complete .bmp file.
bool WrapDib(std::vector<unsigned char>& dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER info{};
    memcpy(&info, dib.data(), sizeof(info));
    if (info.biSize < sizeof(BITMAPINFOHEADER) || info.biSize > dib.size() ||
        info.biWidth <= 0 || info.biHeight == 0 || info.biPlanes != 1)
        return false;
    uint32_t colors = info.biClrUsed;
    if (!colors && info.biBitCount <= 8) colors = 1u << info.biBitCount;
    if (colors > 256) return false;
    uint32_t masks = 0;
    if (info.biSize == sizeof(BITMAPINFOHEADER) && info.biCompression == BI_BITFIELDS) masks = 12;
    const uint32_t bits_offset = 14 + info.biSize + masks + colors * 4;
    if (bits_offset - 14 >= dib.size()) return false;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42; // "BM"
    header.bfSize = static_cast<DWORD>(14 + dib.size());
    header.bfOffBits = bits_offset;
    std::vector<unsigned char> file(14 + dib.size());
    memcpy(file.data(), &header, 14);
    memcpy(file.data() + 14, dib.data(), dib.size());
    dib.swap(file);
    return true;
}

} // namespace

bool ExtractDwgThumbnail(const std::wstring& path, UINT max_edge,
                         std::vector<unsigned char>& pixels,
                         UINT& width, UINT& height, UINT& stride,
                         UINT& source_width, UINT& source_height,
                         std::wstring* error) {
    pixels.clear();
    width = height = stride = source_width = source_height = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, L"dwg-open-failed");
        return false;
    }
    struct FileOwner { HANDLE h; ~FileOwner() { CloseHandle(h); } } owner{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0x80) {
        SetError(error, L"dwg-not-dwg");
        return false;
    }
    const ULONGLONG file_bytes = static_cast<ULONGLONG>(size.QuadPart);

    unsigned char header[0x80]{};
    if (!ReadAt(file, 0, header, sizeof(header)) || memcmp(header, "AC10", 4) != 0 ||
        header[4] < '0' || header[4] > '9' || header[5] < '0' || header[5] > '9') {
        SetError(error, L"dwg-not-dwg");
        return false;
    }
    const int version = (header[4] - '0') * 10 + (header[5] - '0'); // AC10xx
    if (version < 12) { // R12 and older keep no seeker at 0x0D
        SetError(error, L"dwg-no-preview");
        return false;
    }
    const uint32_t block = U32(header + 0x0D);
    unsigned char intro[16 + 4 + 1]{};
    if (!block || block + sizeof(intro) > file_bytes ||
        !ReadAt(file, block, intro, sizeof(intro)) ||
        memcmp(intro, kImageSentinel, sizeof(kImageSentinel)) != 0) {
        SetError(error, L"dwg-no-preview");
        return false;
    }
    const unsigned count = intro[20];
    if (!count || count > 16) {
        SetError(error, L"dwg-no-preview");
        return false;
    }
    std::vector<unsigned char> table(count * 9u);
    if (!ReadAt(file, block + sizeof(intro), table.data(), static_cast<DWORD>(table.size()))) {
        SetError(error, L"dwg-no-preview");
        return false;
    }
    // PNG (sharper, R2013+) wins over the 8-bit BMP when both are present.
    uint32_t bmp_start = 0, bmp_size = 0, png_start = 0, png_size = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char* e = table.data() + i * 9u;
        const uint32_t start = U32(e + 1), bytes = U32(e + 5);
        if (!bytes || bytes > kMaxImageBytes || start + static_cast<ULONGLONG>(bytes) > file_bytes)
            continue;
        if (e[0] == 2) { bmp_start = start; bmp_size = bytes; }
        else if (e[0] == 6) { png_start = start; png_size = bytes; }
    }
    const bool png = png_size != 0;
    const uint32_t start = png ? png_start : bmp_start;
    const uint32_t bytes = png ? png_size : bmp_size;
    if (!bytes) {
        SetError(error, L"dwg-no-preview");
        return false;
    }
    std::vector<unsigned char> encoded(bytes);
    if (!ReadAt(file, start, encoded.data(), bytes) || (!png && !WrapDib(encoded)) ||
        !DecodeWithWic(encoded, max_edge, pixels, width, height, stride,
                       source_width, source_height)) {
        pixels.clear();
        width = height = stride = source_width = source_height = 0;
        SetError(error, L"dwg-preview-decode-failed");
        return false;
    }
    return true;
}

} // namespace pulse::preview
