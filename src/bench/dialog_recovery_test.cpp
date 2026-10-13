#ifdef PULSE_WITH_SELFTEST
#include "../ui/dialog_recovery_test.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>

namespace {
bool HasRenderedContent(const std::wstring& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || !width || !height || width > 4096 || height > 4096) return false;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
    size_t visible = 0, different = 0;
    const auto first = pixels.data();
    for (size_t i = 0; i < pixels.size(); i += 4) {
        visible += pixels[i + 3] > 64;
        different += pixels[i + 3] > 64 && (pixels[i] != first[0] || pixels[i + 1] != first[1] || pixels[i + 2] != first[2]);
    }
    return visible > static_cast<size_t>(width) * height / 2 && different > 100;
}
}


bool RunDialogRecoveryTest() {
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/m14078_dialog_recovery_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        fflush(log); failures += !ok;
    };
    const auto folder = std::wstring(L"bench_data/dialog-recovery-") + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    const bool own_folder = CreateDirectoryW(folder.c_str(), nullptr) != FALSE;
    check(own_folder, "exclusive screenshot output directory created");
    if (!own_folder) { fclose(log); return false; }
    fprintf(log, "Screenshot directory: %ls\n", std::filesystem::absolute(folder).c_str());
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Dialog recovery fixture", WS_POPUP,
        0, 0, 1200, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(owner != nullptr, "private owner created without activating foreground");
    if (owner) {
        for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f, 2.0f}) {
            const auto suffix = std::wstring(dark ? L"dark-" : L"light-") +
                std::to_wstring(static_cast<int>(scale * 100));
            fprintf(log, "Scenario: %s %.0f%%\n", dark ? "dark" : "light", scale * 100);
            pulse::ui::TestAdvancedSearchRecovery(owner, scale, dark, folder + L"/advanced-" + suffix + L".png", check);
            pulse::ui::TestColorPickerFallback(owner, scale, dark, folder + L"/color-" + suffix + L".png", check);
            check(HasRenderedContent(folder + L"/advanced-" + suffix + L".png"), "advanced screenshot has visible nonuniform rendered pixels");
            check(HasRenderedContent(folder + L"/color-" + suffix + L".png"), "color screenshot has visible nonuniform rendered pixels");
        }
        DestroyWindow(owner);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    fprintf(log, "Failures: %d\n", failures); fclose(log);
    return failures == 0;
}
#endif
