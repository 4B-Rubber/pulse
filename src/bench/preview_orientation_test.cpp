#include "preview_host_client.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <objbase.h>
#include <filesystem>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Check(bool ok, const char* name, bool jpeg, unsigned orientation, unsigned page = 0, unsigned cap = 0) {
    printf("[%s] %s format=%s orientation=%u page=%u cap=%u\n",
        ok ? "PASS" : "FAIL", name, jpeg ? "JPEG" : "TIFF", orientation, page, cap);
    fflush(stdout);
    failures += !ok;
}
constexpr UINT width = 160, height = 96;
constexpr std::array<std::array<BYTE, 3>, 4> colors{{{0,0,255}, {0,255,0}, {255,0,0}, {0,255,255}}};
const wchar_t* MetadataPath(bool jpeg) { return jpeg ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}"; }
bool Write(IWICImagingFactory* factory, const std::filesystem::path& path, bool jpeg,
           const std::vector<unsigned>& orientations) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatTiff, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    for (unsigned page = 0; page < orientations.size(); ++page) {
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(width, height))) return false;
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR) return false;
        if (orientations[page]) {
            ComPtr<IWICMetadataQueryWriter> metadata;
            PROPVARIANT value{}; value.vt = VT_UI2; value.uiVal = static_cast<USHORT>(orientations[page]);
            if (FAILED(frame->GetMetadataQueryWriter(&metadata)) ||
                FAILED(metadata->SetMetadataByName(MetadataPath(jpeg), &value))) return false;
        }
        std::vector<BYTE> pixels(width * height * 3);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x) {
                const unsigned corner = (y >= height / 2 ? 2u : 0u) + (x >= width / 2 ? 1u : 0u);
                const auto& color = colors[(corner + page) % 4];
                std::copy(color.begin(), color.end(), pixels.begin() + (y * width + x) * 3);
            }
        if (FAILED(frame->WritePixels(height, width * 3, static_cast<UINT>(pixels.size()), pixels.data())) ||
            FAILED(frame->Commit())) return false;
    }
    return SUCCEEDED(encoder->Commit());
}
bool VerifyMetadata(IWICImagingFactory* factory, const std::filesystem::path& path,
                    bool jpeg, const std::vector<unsigned>& orientations) {
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder))) return false;
    UINT count = 0;
    if (FAILED(decoder->GetFrameCount(&count)) || count != orientations.size()) return false;
    for (UINT page = 0; page < count; ++page) {
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICMetadataQueryReader> metadata;
        if (FAILED(decoder->GetFrame(page, &frame)) || FAILED(frame->GetMetadataQueryReader(&metadata))) return false;
        PROPVARIANT value{};
        const HRESULT hr = metadata->GetMetadataByName(MetadataPath(jpeg), &value);
        const bool ok = orientations[page] ? SUCCEEDED(hr) && value.vt == VT_UI2 && value.uiVal == orientations[page]
            : hr == WINCODEC_ERR_PROPERTYNOTFOUND;
        PropVariantClear(&value);
        if (!ok) return false;
    }
    return true;
}
void Probe(pulse_test::Host& host, const std::filesystem::path& path, bool jpeg,
           unsigned orientation, unsigned page, unsigned cap) {
    pulse_test::Result result;
    const bool received = host.Request(path.wstring(), result, MAXDWORD, cap,
        pulse::ipc::PreviewRequestKind::Content, 0, page);
    const bool raster = received && result.response.status == 0 &&
        result.response.kind == pulse::ipc::PreviewContentKind::Bitmap &&
        result.pixels.size() >= static_cast<size_t>(result.response.stride) * result.response.height &&
        result.response.width && result.response.height;
    Check(raster, "actual host returns bitmap", jpeg, orientation, page, cap);
    if (!raster) return;
    const bool swap = orientation >= 5;
    const UINT sw = swap ? height : width, sh = swap ? width : height;
    const auto limit = pulse::ipc::ClampPreviewPixelSize(cap, false);
    const double ratio = (std::min)(1.0, static_cast<double>(limit) / width);
    const UINT expected_w = static_cast<UINT>(sw * ratio + 0.5);
    const UINT expected_h = static_cast<UINT>(sh * ratio + 0.5);
    Check(result.response.width == expected_w && result.response.height == expected_h,
        "oriented raster dimensions", jpeg, orientation, page, cap);
    Check(result.response.source_width == sw && result.response.source_height == sh,
        "oriented source dimensions", jpeg, orientation, page, cap);
    constexpr unsigned maps[8][4] = {{0,1,2,3},{1,0,3,2},{3,2,1,0},{2,3,0,1},
        {0,2,1,3},{2,0,3,1},{3,1,2,0},{1,3,0,2}};
    bool pixels_ok = true;
    for (unsigned corner = 0; corner < 4; ++corner) {
        const UINT x = result.response.width * (corner % 2 ? 3 : 1) / 4;
        const UINT y = result.response.height * (corner / 2 ? 3 : 1) / 4;
        const auto* pixel = result.pixels.data() + y * result.response.stride + x * 4;
        const auto& color = colors[(maps[orientation ? orientation - 1 : 0][corner] + page) % 4];
        for (unsigned channel = 0; channel < 3; ++channel)
            pixels_ok &= std::abs(static_cast<int>(pixel[channel]) - color[channel]) <= (jpeg ? 24 : 0);
        pixels_ok &= pixel[3] == 255;
        printf("[PIXEL] corner=%u BGR=%u,%u,%u expected=%u,%u,%u\n", corner,
            pixel[0], pixel[1], pixel[2], color[0], color[1], color[2]);
    }
    Check(pixels_ok, "four corner orientation including mirrors", jpeg, orientation, page, cap);
}
}
int wmain() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)))) return 2;
    namespace fs = std::filesystem;
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto root = parent / (L"orientation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(root)) return 2;
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    pulse_test::Host host;
    if (!host.Start()) return 2;
    for (bool jpeg : {false, true})
        for (unsigned orientation = 0; orientation <= 8; ++orientation) {
            const auto path = root / (std::to_wstring(orientation) + (jpeg ? L".jpg" : L".tif"));
            const bool written = Write(factory.Get(), path, jpeg, {orientation});
            Check(written, "encode private fixture", jpeg, orientation);
            const bool verified = written && VerifyMetadata(factory.Get(), path, jpeg, {orientation});
            Check(verified, "verify stored orientation metadata before host", jpeg, orientation);
            if (!verified) continue;
            for (unsigned cap : {256u, 64u}) Probe(host, path, jpeg, orientation, 0, cap);
        }
    const auto multi = root / L"two-pages.tif";
    const bool verified = Write(factory.Get(), multi, false, {6, 8}) &&
        VerifyMetadata(factory.Get(), multi, false, {6, 8});
    Check(verified, "verify independent directions on both TIFF pages", false, 6);
    if (verified) for (unsigned page = 0; page < 2; ++page)
        for (unsigned cap : {256u, 64u}) Probe(host, multi, false, page ? 8 : 6, page, cap);
    host.Stop();
    factory.Reset();
    if (root.parent_path() != parent || !root.filename().wstring().starts_with(L"orientation-")) return 2;
    std::error_code error;
    fs::remove_all(root, error);
    Check(!error && !fs::exists(root), "private fixtures removed after host exit", false, 0);
    if (SUCCEEDED(com)) CoUninitialize();
    return failures ? 1 : 0;
}
