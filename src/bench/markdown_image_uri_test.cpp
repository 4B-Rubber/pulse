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
    static void Run(Canvas& canvas, const std::filesystem::path& root) {
        const std::vector<std::pair<std::wstring, std::wstring>> valid{
            {L"plain.png", L"plain.png"}, {L"%F0%9F%98%80.png", L"\U0001F600.png"}, {L"a%20b.png", L"a b.png"},
            {L"%E4%B8%AD.png", L"中.png"}, {L"rate%25.png", L"rate%.png"},
            {L"hash%23part.png", L"hash#part.png"}, {L"literal%2520.png", L"literal%20.png"},
            {L"a%20b.png?size=1#fragment", L"a b.png"}};
        for (const auto& [uri, filename] : valid) {
            const auto expected = root / filename;
            canvas.Begin(false); canvas.End();
            Check(SavePng(canvas.dc.Get(), canvas.bitmap.Get(), expected), "create owned PNG URI fixture");
            Case(canvas, root, uri, expected.wstring(), true);
        }
        for (const auto* uri : {L"broken%.png", L"broken%GG.png", L"%C0%AF.png", L"%00.png",
                               L"%ED%A0%80.png", L"%F4%90%80%80.png", L"%E4%B8.png",
                               L"%2F%2Fserver/share/x.png", L"%5C%5Cserver/share/x.png",
                               L"https%3A%2F%2Fexample.invalid/x.png", L"https://example.invalid/x.png"})
            Case(canvas, root, uri, {}, false);
    }
    static void Case(Canvas& canvas, const std::filesystem::path& root, const std::wstring& uri,
                     const std::wstring& expected, bool accepted) {
        ThumbnailCache cache;
        cache.SetDeviceContext(canvas.dc.Get());
        // Metadata uses the production async resolver and owned files; keep the
        // decode worker stopped. Invalid network URIs are rejected before metadata I/O.
        cache.running_ = true;
        MarkdownView view;
        std::wstring payload;
        Check(preview::MakeMarkdownDocument(L"![picture](" + uri + L")", payload), "real md4c accepts fixture");
        Check(view.SetPayload(payload, (root / L"note.md").wstring()), "real MarkdownView loads host payload");
        const auto deadline = GetTickCount64() + 2000;
        do {
            canvas.Begin(false);
            view.Draw(canvas.dc.Get(), canvas.factory.Get(), D2D1::RectF(0, 0, 600, 300),
                MakeTheme(false, HexColor(0x0078d4)), false, 1, &cache, 1, 0, 0, {});
            canvas.End();
            if (!view.blocks_.empty() && view.blocks_[0].image_metadata_ready) break;
            Sleep(5);
        } while (GetTickCount64() < deadline);
        Check(!view.blocks_.empty() && view.blocks_[0].image_metadata_ready,
              "production async metadata resolves or rejects the URI before assertion");
        if (accepted) {
            const bool exact = cache.queue_.size() == 1 &&
                _wcsicmp(cache.queue_.front().path.c_str(), expected.c_str()) == 0;
            printf("[CASE] uri=%ls expected=%ls actual=%ls requests=%zu\n", uri.c_str(), expected.c_str(),
                cache.queue_.empty() ? L"(none)" : cache.queue_.front().path.c_str(), cache.queue_.size());
            Check(exact, "image request resolves URI exactly once to intended owned file");
            if (exact) {
                std::error_code error;
                Check(std::filesystem::equivalent(cache.queue_.front().path, expected, error) && !error,
                    "queued path identifies the actual private PNG");
            }
        } else {
            printf("[CASE] rejected_uri=%ls actual=%ls requests=%zu\n", uri.c_str(),
                cache.queue_.empty() ? L"(none)" : cache.queue_.front().path.c_str(), cache.queue_.size());
            Check(cache.queue_.empty(), "invalid URI or encoded network destination emits no image request");
        }
        cache.running_ = false;
    }
};
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    namespace fs = std::filesystem;
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    fs::create_directories(parent);
    const auto root = parent / (L"markdown-uri-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 2;
    {
        Canvas canvas;
        const bool ready = canvas.Init(1);
        Check(ready, "initialize private WARP and DirectWrite surface");
        if (ready) pulse::ui::ThumbnailCacheTestAccess::Run(canvas, root);
    }
    if (root.parent_path() != parent || !root.filename().wstring().starts_with(L"markdown-uri-")) return 2;
    std::error_code error;
    fs::remove_all(root, error);
    Check(!error && !fs::exists(root), "remove exclusive URI fixtures");
    if (SUCCEEDED(com)) CoUninitialize();
    return failures ? 1 : 0;
}
