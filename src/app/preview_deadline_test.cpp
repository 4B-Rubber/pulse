#ifdef PULSE_WITH_SELFTEST
#include "../ui/quick_preview_window.h"
#include <cstdio>
#include <filesystem>
#include <functional>

namespace pulse::ui {
struct PreviewDeadlineProbe {
    static inline FILE* log = nullptr;
    static inline int failures = 0;
    static void Check(bool ok, const char* text) {
        if (log) { fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", text); fflush(log); }
        failures += !ok;
    }
    static void Pump(unsigned ms) {
        const auto end = GetTickCount64() + ms;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            Sleep(5);
        } while (GetTickCount64() < end);
    }
    static bool Until(const std::function<bool()>& done, unsigned ms = 2500) {
        const auto end = GetTickCount64() + ms;
        do { Pump(10); if (done()) return true; } while (GetTickCount64() < end);
        return false;
    }
    static ThumbnailCache::Request Take(ThumbnailCache& cache) {
        Check(!cache.queue_.empty(), "production Draw enqueues request");
        if (cache.queue_.empty()) return {};
        auto request = cache.queue_.front();
        cache.queue_.pop_front();
        return request;
    }
    static ThumbnailCache::Item Bitmap() {
        ThumbnailCache::Item item;
        item.kind = ipc::PreviewContentKind::Bitmap;
        item.w = item.h = 2; item.stride = 8;
        item.pixels.assign(16, 255);
        return item;
    }
    static void Pages() {
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> context;
        ComPtr<ID2D1Bitmap1> target;
        bool ready = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
            SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(D2D1CreateDevice(dxgi.get(), nullptr, &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        if (ready) {
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ready = SUCCEEDED(context->CreateBitmap(D2D1::SizeU(128, 128), nullptr, 0, props, &target));
        }
        Check(ready, "create isolated WARP page target");
        if (!ready) return;
        auto* dc = context.get();
        dc->SetTarget(target.get());
        dc->BeginDraw();
        ThumbnailCache cache;
        cache.SetDeviceContext(dc);
        cache.running_ = true; // The fixture supplies completions; no external host is started.
        auto draw = [&](unsigned frame, unsigned pixels) {
            return cache.Draw(dc, D2D1::RectF(0, 0, 100, 100), L"private-pages.pdf",
                0, pixels, 1, 2, 3, 1, nullptr, nullptr, nullptr, true, nullptr,
                nullptr, nullptr, nullptr, nullptr, frame);
        };
        draw(0, 128);
        auto first = Take(cache);
        auto bitmap = Bitmap(); bitmap.frame_count = 2;
        Check(cache.StoreResult(first, std::move(bitmap)), "store distinct first page");
        Check(draw(0, 128) == PreviewDrawResult::Bitmap, "first page draws");
        draw(1, 128);
        auto second = Take(cache);
        ThumbnailCache::Item failed; failed.failed = true;
        cache.StoreResult(second, std::move(failed));
        Check(draw(1, 128) == PreviewDrawResult::Failed, "failed second page never draws page one");
        draw(1, 256); second = Take(cache);
        failed = {}; failed.failed = failed.transient = true;
        cache.StoreResult(second, std::move(failed));
        Check(draw(1, 256) == PreviewDrawResult::Pending, "transient second page never draws page one");
        Check(cache.TakeRetryDeadline() > GetTickCount64(), "retry exposes its future deadline");
        Check(draw(0, 256) == PreviewDrawResult::Bitmap, "first page preserves resize fallback");
        Check(SUCCEEDED(dc->EndDraw()), "page fallback WARP drawing completes");
        cache.running_ = false;
    }
    static bool Run() {
        failures = 0;
        wchar_t layout_only[8]{};
        if (GetEnvironmentVariableW(L"PULSE_TEST_FIND_LAYOUT", layout_only, ARRAYSIZE(layout_only)) &&
            wcscmp(layout_only, L"1") == 0) {
            _wfopen_s(&log, L"bench_data/quick_preview_find_layout_test.log", L"w");
            Check(log != nullptr, "open find layout evidence log");
            QuickPreviewWindow q;
            for (float scale : {1.0f, 1.5f, 2.0f}) {
                q.scale_ = scale;
                for (auto kind : {QuickPreviewWindow::NativeKind::Text, QuickPreviewWindow::NativeKind::Hex,
                    QuickPreviewWindow::NativeKind::Archive, QuickPreviewWindow::NativeKind::Markdown,
                    QuickPreviewWindow::NativeKind::Table, QuickPreviewWindow::NativeKind::Tree}) {
                    q.native_kind_ = kind;
                    for (float notice : {0.0f, 48.0f * scale}) {
                        q.preview_notice_height_ = notice;
                        q.find_open_ = false;
                        const float closed_top = q.ContentRect().top;
                        Check(q.FindBarHeight() == 0, "closed search reserves no find bar");
                        q.find_open_ = true;
                        const auto bar = q.FindBarRect();
                        const auto edit = q.FindEditCell();
                        Check(q.FindBarHeight() == bar.bottom - bar.top && q.FindBarHeight() > 0,
                            "every supported preview reserves full visible find bar");
                        Check(edit.top >= bar.top && edit.bottom <= bar.bottom,
                            "hosted edit stays vertically inside reserved find bar");
                        Check(q.ContentRect().top == bar.bottom + notice,
                            "content starts after find bar and complete warning height");
                        Check(q.ContentRect().top == closed_top + q.FindBarHeight(),
                            "opening find shifts content by reserved bar height");
                        q.find_open_ = false;
                        Check(q.ContentRect().top == closed_top, "closing find restores content position");
                    }
                }
            }
            if (log) { fprintf(log, "Failures: %d\n", failures); fclose(log); }
            log = nullptr;
            return failures == 0;
        }
        std::filesystem::create_directories(L"bench_data");
        _wfopen_s(&log, L"bench_data/m17_preview_deadline_test.log", L"w");
        Check(log != nullptr, "open test evidence log");
        const auto base = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"m17-preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directory(base);
        HWND owner = CreateWindowExW(0, L"STATIC", L"Private preview deadline test", WS_POPUP,
            0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(owner != nullptr, "create private owner");
        {
            QuickPreviewWindow q;
            const bool initialized = q.Initialize(owner, WM_APP + 211, WM_APP + 212);
            Check(initialized, "initialize real Quick Preview window");
            if (initialized) {
                q.thumbnails_.running_ = true;
                QuickPreviewItem item;
                item.path = (base / L"fixture.png").wstring(); item.name = L"Private fixture";
                q.Show(item, false, WindowEffect::None, true);
                Pump(50);
                auto request = Take(q.thumbnails_);
                ThumbnailCache::Item failed; failed.failed = failed.transient = true;
                q.thumbnails_.StoreResult(request, std::move(failed));
                // Shorten only this private item's budget; use the real Draw/timer path.
                q.thumbnails_.items_.at(request.key).retry_at = GetTickCount64() + 180;
                InvalidateRect(q.hwnd_, nullptr, FALSE);
                Pump(30);
                Check(q.preview_deadline_ != 0, "failure paint arms a deadline");
                Check(Until([&] { return !q.thumbnails_.queue_.empty(); }), "idle message pump automatically retries");
                request = Take(q.thumbnails_);
                q.thumbnails_.StoreResult(request, Bitmap());
                InvalidateRect(q.hwnd_, nullptr, FALSE);
                Pump(60);
                Check(q.preview_deadline_ == 0 && q.thumbnails_.queue_.empty(), "success removes deadline and pending work");
                Check(!KillTimer(q.hwnd_, 74), "successful static preview has no retry timer");
                Pump(250);
                Check(q.preview_deadline_ == 0 && q.thumbnails_.queue_.empty(), "successful static preview stays idle");
                q.handler_.PrimeStalledOpenForTest();
                InvalidateRect(q.hwnd_, nullptr, FALSE);
                Pump(30);
                Check(q.preview_deadline_ != 0, "stalled provider state arms owner deadline");
                Check(Until([&] { return q.handler_.NextDeadline() == 0; }),
                    "owner message pump retires stalled provider without periodic test Sync");
                Pages();

                item.path = (base / L"fixture.xlsx").wstring();
                q.Update(item); Pump(40);
                request = Take(q.thumbnails_);
                ThumbnailCache::Item sheet;
                sheet.kind = ipc::PreviewContentKind::Table;
                sheet.text = L"PULSETBL\t1\nS\txlsx\tFirst\t1\t1\t0\t\t0\nR\tfirst\n"
                    L"S\txlsx\tSecond\t0\t0\t0\tnot-loaded\t0\nW\t2\t1\t0\n";
                q.thumbnails_.StoreResult(request, std::move(sheet));
                InvalidateRect(q.hwnd_, nullptr, FALSE); Pump(40);
                Check(q.table_.HasData(), "real Render loads initial sheet");
                q.table_.Key(VK_NEXT, false, true);
                InvalidateRect(q.hwnd_, nullptr, FALSE); Pump(40);
                request = Take(q.thumbnails_);
                Check(request.frame_index == 1 && q.table_.IsSheetRequestPending(), "real Render requests second sheet");
                failed = {}; failed.failed = true;
                q.thumbnails_.StoreResult(request, std::move(failed));
                InvalidateRect(q.hwnd_, nullptr, FALSE); Pump(50);
                Check(q.sheet_request_ == 1 && q.table_.SelectedSheetIndex() == 1 &&
                    !q.table_.IsSheetRequestPending(), "terminal failure preserves selected sheet and ends loading");
                q.ResetAnimation();
                Check(q.sheet_request_ == 1, "animation reset preserves sheet identity");
                q.thumbnails_.items_.at(request.key).transient = true;
                q.thumbnails_.items_.at(request.key).retry_at = GetTickCount64() + 150;
                InvalidateRect(q.hwnd_, nullptr, FALSE); Pump(30);
                Check(q.preview_deadline_ != 0, "real failed sheet arms retry before close");
                q.Close(); Pump(180);
                Check(q.preview_deadline_ == 0 && q.thumbnails_.queue_.empty(), "Close cancels deadline and queued work");
                Check(!KillTimer(q.hwnd_, 74), "closed preview has no retry timer");
                q.thumbnails_.running_ = false;
            }
        }
        if (owner) DestroyWindow(owner);
        std::error_code error;
        Check(std::filesystem::remove(base, error) && !error, "remove private empty fixture directory");
        if (log) fclose(log);
        log = nullptr;
        return failures == 0;
    }
};
}
bool RunPreviewDeadlineTest() { return pulse::ui::PreviewDeadlineProbe::Run(); }
#endif
