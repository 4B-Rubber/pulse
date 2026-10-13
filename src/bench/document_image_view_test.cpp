#include "../ui/markdown_view.h"
#include "../ui/thumbnail_cache.h"
#include "../preview_host/markdown_document.h"
#include <d3d11.h>
#include <wincodec.h>
#include <objbase.h>
#include <string_view>
#include <filesystem>
#include <cstdio>
#include <algorithm>
using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Check(bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
bool SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const std::filesystem::path& path) {
    ComPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        bitmap->GetPixelFormat());
    const auto size = bitmap->GetPixelSize();
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(size.width, size.height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(size.height, mapped.pitch, mapped.pitch * size.height, mapped.bits)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    readable->Unmap();
    return ok;
}
struct Canvas {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<IDWriteFactory2> factory;
    bool Init(float scale) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) ||
            FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) || FAILED(d3d.As(&dxgi)) ||
            FAILED(D2D1CreateDevice(dxgi.Get(), nullptr, &device)) ||
            FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(600 * scale), static_cast<UINT32>(300 * scale)),
            nullptr, 0, props, &bitmap))) return false;
        dc->SetTarget(bitmap.Get());
        return true;
    }
    void Begin(bool dark) { dc->BeginDraw(); dc->Clear(D2D1::ColorF(dark ? 0x17191c : 0xffffff)); }
    void End() { Check(SUCCEEDED(dc->EndDraw()), "D2D drawing completes"); }
    std::vector<unsigned char> Pixels() {
        ComPtr<ID2D1Bitmap1> copy;
        const auto size = bitmap->GetPixelSize();
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, bitmap->GetPixelFormat());
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &copy)) ||
            FAILED(copy->CopyFromBitmap(nullptr, bitmap.Get(), nullptr))) return {};
        D2D1_MAPPED_RECT map{};
        if (FAILED(copy->Map(D2D1_MAP_OPTIONS_READ, &map))) return {};
        std::vector<unsigned char> bytes(size.width * size.height * 4);
        for (UINT32 y = 0; y < size.height; ++y)
            std::copy_n(map.bits + y * map.pitch, size.width * 4, bytes.data() + y * size.width * 4);
        copy->Unmap();
        return bytes;
    }
};
}
namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static void Run(Canvas& canvas, float scale, bool dark, const std::filesystem::path& root) {
        canvas.Begin(dark); canvas.End();
        for (const auto* name : {L"visible-private.png", L"far-private.png", L"first-private.png", L"second-private.png"})
            Check(SavePng(canvas.dc.Get(), canvas.bitmap.Get(), root / name), "create owned document image fixture");
        ThumbnailCache cache; cache.SetDeviceContext(canvas.dc.Get()); cache.running_ = true;
        MarkdownView view;
        std::wstring source = L"![Document picture](visible-private.png)\n\nFollowing text after the image.\n\n";
        for (int i = 0; i < 60; ++i) source += L"A paragraph outside the initial viewport.\n\n";
        source += L"![Far picture](far-private.png)\n";
        std::wstring payload; preview::MakeMarkdownDocument(source, payload);
        Check(view.SetPayload(payload, (root / L"note.md").wstring()) &&
            !view.blocks_.empty() && view.blocks_[0].image_path.empty() && !view.blocks_[0].image_metadata_ready && cache.queue_.empty(),
            "SetPayload defers image metadata and decode scheduling");
        const auto rect = D2D1::RectF(0, 0, 600 * scale, 300 * scale);
        const auto theme = MakeTheme(dark, HexColor(0x0078d4));
        auto draw = [&] {
            canvas.Begin(dark);
            view.Draw(canvas.dc.Get(), canvas.factory.Get(), rect, theme, dark, scale, &cache, 1, 0, 0, {});
            canvas.End();
        };
        auto wait_metadata = [&] {
            const auto deadline = GetTickCount64() + 2000;
            do {
                draw();
                if (!view.blocks_.empty() && view.blocks_[0].image_metadata_ready) return true;
                Sleep(5);
            } while (GetTickCount64() < deadline);
            return false;
        };
        Check(wait_metadata(), "visible image metadata completes through production async resolver");
        Check(cache.queue_.size() == 1 && cache.queue_.front().path.find(L"visible-private.png") != std::wstring::npos,
            "only the visible image enters the cache queue");
        if (cache.queue_.empty()) { cache.running_ = false; return; }
        const auto before = canvas.Pixels();
        const auto stem = L"m16006-image-" + std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark" : L"-light");
        Check(SavePng(canvas.dc.Get(), canvas.bitmap.Get(), std::filesystem::path(L"bench_data") / (stem + L"-pending.png")),
            "save placeholder rendering");
        const auto request = cache.queue_.front(); cache.queue_.clear();
        ThumbnailCache::Item bitmap;
        bitmap.kind = ipc::PreviewContentKind::Bitmap; bitmap.w = 120; bitmap.h = 40; bitmap.stride = 480;
        bitmap.source_width = 240; bitmap.source_height = 80;
        bitmap.pixels.resize(480 * 40);
        for (size_t i = 0; i < bitmap.pixels.size(); i += 4) {
            bitmap.pixels[i] = 210; bitmap.pixels[i + 1] = 130; bitmap.pixels[i + 2] = 30; bitmap.pixels[i + 3] = 255;
        }
        Check(cache.StoreResult(request, std::move(bitmap)), "current image dimensions publish through real cache");
        draw(); draw();
        Check(view.blocks_[0].image_aspect > 0.32f && view.blocks_[0].image_aspect < 0.34f &&
            view.blocks_[1].box_top >= view.blocks_[0].box_bottom && canvas.Pixels() != before,
            "real bitmap dimensions relayout without overlapping following text");
        Check(SavePng(canvas.dc.Get(), canvas.bitmap.Get(), std::filesystem::path(L"bench_data") / (stem + L"-loaded.png")),
            "save loaded image rendering");
        const auto lease = view.image_session_;
        view.Clear();
        Check(!lease->active.load(), "Clear immediately invalidates document image lifetime");
        ThumbnailCache::Item stale;
        stale.kind = ipc::PreviewContentKind::Bitmap;
        Check(!cache.StoreResult(request, std::move(stale)), "cleared document session rejects late decode result");
        std::wstring first, second;
        preview::MakeMarkdownDocument(L"![First](first-private.png)", first);
        preview::MakeMarkdownDocument(L"![Second](second-private.png)", second);
        const auto chapters = L"PULSEMD\t1\nP\tFirst\n" + first.substr(first.find(L'\n') + 1) +
            L"P\tSecond\n" + second.substr(second.find(L'\n') + 1);
        Check(view.SetPayload(chapters, (root / L"book.md").wstring()) && view.SectionCount() == 2, "chapter image fixture parsed");
        Check(wait_metadata(), "selected chapter image metadata completes asynchronously");
        Check(cache.queue_.size() == 1 && cache.queue_.front().path.find(L"first-private.png") != std::wstring::npos,
            "unopened chapters do not query image metadata");
        auto lifetime = view.image_session_;
        { MarkdownView temporary; temporary.SetPayload(first, (root / L"temporary.md").wstring()); lifetime = temporary.image_session_; }
        Check(!lifetime->active.load(), "view destruction invalidates outstanding image work");
        cache.running_ = false;
    }
};
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    namespace fs = std::filesystem;
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    fs::create_directories(parent);
    const auto root = parent / (L"document-images-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 2;
    for (float scale : {1.0f, 1.5f}) {
        Canvas canvas; const bool ready = canvas.Init(scale); Check(ready, "WARP image view initialized");
        if (ready) for (bool dark : {false, true}) pulse::ui::ThumbnailCacheTestAccess::Run(canvas, scale, dark, root);
    }
    if (root.parent_path() != parent || !root.filename().wstring().starts_with(L"document-images-")) return 2;
    std::error_code cleanup;
    fs::remove_all(root, cleanup);
    Check(!cleanup && !fs::exists(root), "owned document image fixtures cleaned up");
    if (SUCCEEDED(com)) CoUninitialize();
    return failures ? 1 : 0;
}
