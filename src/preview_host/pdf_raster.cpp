// pdf_raster.cpp — see pdf_raster.h.
#include "pdf_raster.h"
#include "../../third_party/pdfium/include/fpdfview.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace pulse::preview {
namespace {

// The preview host serves requests on one thread, so the library is set up
// once on first use and kept for the life of the process.
struct PdfApi {
    HMODULE module = nullptr;
#define PDF_FN(name) decltype(&name) name##_fn = nullptr
    PDF_FN(FPDF_InitLibraryWithConfig); PDF_FN(FPDF_LoadCustomDocument);
    PDF_FN(FPDF_CloseDocument); PDF_FN(FPDF_GetLastError);
    PDF_FN(FPDF_GetPageCount); PDF_FN(FPDF_LoadPage); PDF_FN(FPDF_ClosePage);
    PDF_FN(FPDF_GetPageWidthF); PDF_FN(FPDF_GetPageHeightF);
    PDF_FN(FPDF_RenderPageBitmap); PDF_FN(FPDFBitmap_CreateEx);
    PDF_FN(FPDFBitmap_FillRect); PDF_FN(FPDFBitmap_GetBuffer);
    PDF_FN(FPDFBitmap_GetStride); PDF_FN(FPDFBitmap_Destroy);
#undef PDF_FN
    bool ready = false;

    PdfApi() {
        wchar_t exe[32768]{};
        const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        if (!n || n >= ARRAYSIZE(exe)) return;
        std::wstring dll(exe, n);
        const size_t slash = dll.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        dll.resize(slash + 1);
        dll += L"pdfium.dll";
        module = LoadLibraryExW(dll.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
#define PDF_LOAD(name) name##_fn = reinterpret_cast<decltype(name##_fn)>(GetProcAddress(module, #name)); if (!name##_fn) return
        PDF_LOAD(FPDF_InitLibraryWithConfig); PDF_LOAD(FPDF_LoadCustomDocument);
        PDF_LOAD(FPDF_CloseDocument); PDF_LOAD(FPDF_GetLastError);
        PDF_LOAD(FPDF_GetPageCount); PDF_LOAD(FPDF_LoadPage); PDF_LOAD(FPDF_ClosePage);
        PDF_LOAD(FPDF_GetPageWidthF); PDF_LOAD(FPDF_GetPageHeightF);
        PDF_LOAD(FPDF_RenderPageBitmap); PDF_LOAD(FPDFBitmap_CreateEx);
        PDF_LOAD(FPDFBitmap_FillRect); PDF_LOAD(FPDFBitmap_GetBuffer);
        PDF_LOAD(FPDFBitmap_GetStride); PDF_LOAD(FPDFBitmap_Destroy);
#undef PDF_LOAD
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        FPDF_InitLibraryWithConfig_fn(&config);
        ready = true;
    }
};

PdfApi& Api() {
    static PdfApi api;
    return api;
}

// Buffered random-access reader: PDFium issues many small reads while it parses
// the cross-reference table and the first page's objects.
struct Reader {
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned long size = 0;
    std::array<unsigned char, 64 * 1024> cache{};
    unsigned long offset = 0;
    DWORD available = 0;

    static int Read(void* context, unsigned long position, unsigned char* data,
                    unsigned long count) {
        auto& self = *static_cast<Reader*>(context);
        if (position > self.size || count > self.size - position) return 0;
        while (count) {
            if (position < self.offset || position >= self.offset + self.available) {
                self.offset = position;
                self.available = 0;
                LARGE_INTEGER at{};
                at.QuadPart = position;
                if (!SetFilePointerEx(self.file, at, nullptr, FILE_BEGIN)) return 0;
                const DWORD want = static_cast<DWORD>((std::min)(
                    static_cast<unsigned long>(self.cache.size()), self.size - position));
                if (!ReadFile(self.file, self.cache.data(), want, &self.available, nullptr) ||
                    !self.available)
                    return 0;
            }
            const DWORD from = position - self.offset;
            const DWORD n = (std::min)(static_cast<DWORD>(count), self.available - from);
            memcpy(data, self.cache.data() + from, n);
            count -= n;
            position += n;
            data += n;
        }
        return 1;
    }
};

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

// Preview budget: very large sets (plotted drawing sets, scanned books) still
// open quickly because PDFium reads lazily, but the 32-bit FPDF_FILEACCESS
// length caps what it can address.
constexpr ULONGLONG kMaxPdfBytes = 1024ull * 1024 * 1024;
constexpr UINT kMaxRenderEdge = 4096;

} // namespace

bool RasterizePdfFile(const std::wstring& path, UINT max_edge,
                      std::vector<unsigned char>& pixels,
                      UINT& width, UINT& height, UINT& stride,
                      UINT& source_width, UINT& source_height,
                      std::wstring* error) {
    pixels.clear();
    width = height = stride = source_width = source_height = 0;
    PdfApi& api = Api();
    if (!api.ready) {
        SetError(error, L"pdfium-unavailable");
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, L"pdf-open-failed");
        return false;
    }
    struct FileOwner { HANDLE h; ~FileOwner() { CloseHandle(h); } } file_owner{file};
    LARGE_INTEGER bytes{};
    if (!GetFileSizeEx(file, &bytes) || bytes.QuadPart < 8) {
        SetError(error, L"pdf-open-failed");
        return false;
    }
    if (static_cast<ULONGLONG>(bytes.QuadPart) > kMaxPdfBytes) {
        SetError(error, L"pdf-too-large");
        return false;
    }

    // Legacy Illustrator files (and EPS renamed to .ai) are PostScript; PDFium
    // would only report a format error after scanning the whole file.
    char head[1024]{};
    DWORD got = 0;
    if (!ReadFile(file, head, sizeof(head), &got, nullptr) || got < 5) {
        SetError(error, L"pdf-open-failed");
        return false;
    }
    bool has_marker = false;
    for (DWORD i = 0; i + 5 <= got; ++i) {
        if (memcmp(head + i, "%PDF-", 5) == 0) { has_marker = true; break; }
    }
    if (!has_marker) {
        SetError(error, L"pdf-not-pdf");
        return false;
    }

    auto reader = std::make_unique<Reader>();
    reader->file = file;
    reader->size = static_cast<unsigned long>(bytes.QuadPart);
    FPDF_FILEACCESS access{reader->size, Reader::Read, reader.get()};
    FPDF_DOCUMENT doc = api.FPDF_LoadCustomDocument_fn(&access, nullptr);
    if (!doc) {
        SetError(error, api.FPDF_GetLastError_fn() == FPDF_ERR_PASSWORD
            ? L"pdf-password" : L"pdf-load-failed");
        return false;
    }
    struct DocOwner { PdfApi& api; FPDF_DOCUMENT d; ~DocOwner() { api.FPDF_CloseDocument_fn(d); } }
        doc_owner{api, doc};
    if (api.FPDF_GetPageCount_fn(doc) <= 0) {
        SetError(error, L"pdf-empty");
        return false;
    }
    FPDF_PAGE page = api.FPDF_LoadPage_fn(doc, 0);
    if (!page) {
        SetError(error, L"pdf-page-failed");
        return false;
    }
    struct PageOwner { PdfApi& api; FPDF_PAGE p; ~PageOwner() { api.FPDF_ClosePage_fn(p); } }
        page_owner{api, page};

    // Page size in points (1/72 in), already accounting for /Rotate.
    const float page_w = api.FPDF_GetPageWidthF_fn(page);
    const float page_h = api.FPDF_GetPageHeightF_fn(page);
    if (!(page_w > 0.5f) || !(page_h > 0.5f) || !std::isfinite(page_w) || !std::isfinite(page_h)) {
        SetError(error, L"pdf-page-failed");
        return false;
    }
    source_width = static_cast<UINT>(std::lround(page_w * 96.0f / 72.0f));
    source_height = static_cast<UINT>(std::lround(page_h * 96.0f / 72.0f));

    const UINT edge = (std::clamp)(max_edge ? max_edge : 256u, 16u, kMaxRenderEdge);
    const float scale = static_cast<float>(edge) / (std::max)(page_w, page_h);
    const int bw = (std::max)(1, static_cast<int>(std::lround(page_w * scale)));
    const int bh = (std::max)(1, static_cast<int>(std::lround(page_h * scale)));

    FPDF_BITMAP bitmap = api.FPDFBitmap_CreateEx_fn(bw, bh, FPDFBitmap_BGRA, nullptr, 0);
    if (!bitmap) {
        SetError(error, L"pdf-render-failed");
        return false;
    }
    struct BitmapOwner { PdfApi& api; FPDF_BITMAP b; ~BitmapOwner() { api.FPDFBitmap_Destroy_fn(b); } }
        bitmap_owner{api, bitmap};
    // Paper is white regardless of theme; PDF content assumes it. Every pixel
    // stays opaque, so straight BGRA equals the premultiplied form WIC returns.
    api.FPDFBitmap_FillRect_fn(bitmap, 0, 0, bw, bh, 0xFFFFFFFF);
    api.FPDF_RenderPageBitmap_fn(bitmap, page, 0, 0, bw, bh, 0, FPDF_ANNOT);

    const auto* buffer = static_cast<const unsigned char*>(api.FPDFBitmap_GetBuffer_fn(bitmap));
    const int src_stride = api.FPDFBitmap_GetStride_fn(bitmap);
    if (!buffer || src_stride < bw * 4) {
        SetError(error, L"pdf-render-failed");
        return false;
    }
    width = static_cast<UINT>(bw);
    height = static_cast<UINT>(bh);
    stride = width * 4;
    pixels.resize(static_cast<size_t>(stride) * height);
    for (UINT y = 0; y < height; ++y) {
        const unsigned char* src = buffer + static_cast<size_t>(src_stride) * y;
        unsigned char* dst = pixels.data() + static_cast<size_t>(stride) * y;
        memcpy(dst, src, stride);
        for (UINT x = 0; x < width; ++x) dst[x * 4 + 3] = 0xFF;
    }
    return true;
}

} // namespace pulse::preview
