#include "../ui/ui_renderer.h"
#include "../ui/bloom_accent_picker.h"
#include "../ui/edit_host.h"
#include "../ui/typography.h"
#include "../common/localization.h"
#include <commctrl.h>
#include <cstdio>
#include <cmath>
#include <thread>

namespace pulse::ui {
bool TestAdvancedSearchDeviceRecovery(HWND host);
bool TestColorPickerEditFallback(HWND host, Compositor& compositor);

struct UiModuleAuditTest {
    static bool Typography(Compositor& compositor) {
        MainRenderer warm;
        warm.SetCompositor(&compositor);
        bool ok = true;
        for (int percent : {100,125,90}) {
            typography::SetUiFontScale(percent);
            compositor.RecreateTextFormats(1.0f);
            warm.InvalidateTypography();
            MainRenderer fresh;
            fresh.SetCompositor(&compositor);
            for (const auto* text : {L"C:\\folder\\路径\\file.txt", L"2026-12-30 23:59", L"XLSX"})
                ok &= std::abs(warm.CellTextWidth(text) - fresh.CellTextWidth(text)) < 0.01f;
            const auto a = warm.AutoColumnWidths(), b = fresh.AutoColumnWidths();
            ok &= a.date == b.date && a.type == b.type && a.size == b.size &&
                a.created == b.created && a.accessed == b.accessed;
        }
        typography::SetUiFontScale(100);
        compositor.RecreateTextFormats(1.0f);
        return ok;
    }
};
}

namespace {
bool Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    return ok;
}
LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& compositor = *reinterpret_cast<pulse::ui::Compositor*>(data);
    const auto fg = D2D1::ColorF(1,1,1), bg = D2D1::ColorF(0,0,0);
    LRESULT result = 0;
    if (pulse::ui::HandleChildEditMessage(compositor, compositor.TextFormat(), fg, bg, nullptr,
        hwnd, msg, wp, lp, result)) return result;
    return pulse::ui::DefPresentedChildEditProc(compositor, compositor.TextFormat(), fg, bg, hwnd,msg,wp,lp);
}
bool Selection(HWND host, pulse::ui::Compositor& compositor) {
    const std::wstring text = L"abcdefghijklmnopqrst";
    HWND edit = pulse::ui::CreateChildEdit(host, text.c_str());
    if (!edit) return false;
    SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
    SetWindowPos(edit, nullptr, 0,0,400,32,SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    IDWriteTextLayout* layout = nullptr;
    compositor.DwriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
        compositor.TextFormat(), 1000000.0f, 32.0f, &layout);
    if (!layout) { DestroyWindow(edit); return false; }
    auto click = [&](int index, bool shift) {
        float x = 0,y = 0;
        DWRITE_HIT_TEST_METRICS hit{};
        layout->HitTestTextPosition(index,FALSE,&x,&y,&hit);
        const auto point = MAKELPARAM(static_cast<int>(x+2.0f+hit.width*0.2f),4);
        SendMessageW(edit,WM_LBUTTONDOWN,shift?MK_SHIFT:0,point);
        SendMessageW(edit,WM_LBUTTONUP,0,point);
    };
    auto selected = [&](DWORD start,DWORD end) {
        DWORD lo=0,hi=0;
        SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&lo),reinterpret_cast<LPARAM>(&hi));
        return lo==start && hi==end;
    };
    click(5,false);
    SendMessageW(edit,WM_KEYDOWN,VK_HOME,0);
    click(10,true);
    bool ok = Check(selected(0,10),"native Home then Shift-click uses current anchor");
    SendMessageW(edit,EM_SETSEL,10,2);
    click(5,true);
    ok &= Check(selected(5,10),"reverse programmatic selection preserves its active end and anchor");
    SendMessageW(edit,EM_SETSEL,0,-1);
    click(5,true);
    ok &= Check(selected(0,5),"select-all then Shift-click uses current anchor");
    SendMessageW(edit,WM_KEYDOWN,VK_END,0);
    click(10,true);
    ok &= Check(selected(10,20),"native End then Shift-click uses current anchor");
    click(2,false);
    SetWindowPos(edit,nullptr,0,0,100,32,SWP_NOZORDER|SWP_NOACTIVATE);
    SendMessageW(edit,WM_KEYDOWN,VK_END,0);
    float end_x=0,end_y=0;
    DWRITE_HIT_TEST_METRICS end_hit{};
    layout->HitTestTextPosition(20,FALSE,&end_x,&end_y,&end_hit);
    const float scroll = std::max(0.0f,end_x-98.0f);
    BOOL trailing=FALSE,inside=FALSE;
    DWRITE_HIT_TEST_METRICS clicked{};
    layout->HitTestPoint(50.0f+scroll,4.0f,&trailing,&inside,&clicked);
    const DWORD expected = clicked.textPosition + (trailing ? clicked.length : 0);
    SendMessageW(edit,WM_LBUTTONDOWN,0,MAKELPARAM(52,4));
    SendMessageW(edit,WM_LBUTTONUP,0,MAKELPARAM(52,4));
    ok &= Check(selected(expected,expected),"End horizontal scroll and click share displayed DirectWrite coordinates");
    layout->Release();
    DestroyWindow(edit);
    return ok;
}
bool RunOnDesktop() {
    using namespace pulse::ui;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    HWND host = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_NOACTIVATE,L"STATIC",L"",
        WS_POPUP,-30000,-30000,900,800,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    bool ok = host != nullptr;
    if (host) {
        ShowWindow(host,SW_SHOWNOACTIVATE);
        {
            Compositor compositor;
            ok &= Check(compositor.Init(host),"initialize private-desktop graphics host");
            compositor.RecreateTextFormats(1.0f);
            if (compositor.Dc() && compositor.TextFormat()) {
                ok &= Check(UiModuleAuditTest::Typography(compositor),"warm list cell and column caches match fresh renderer at 100/125/90 percent");
                ok &= Selection(host,compositor);
                ok &= Check(TestColorPickerEditFallback(host,compositor),"five actual color editors survive repeated presentation failure at 100/150/200 DPI");
                BloomAccentPicker picker;
                picker.SetDisk(D2D1::RectF(0,0,300,300));
                ID2D1Factory* factory = nullptr;
                compositor.Dc()->GetFactory(&factory);
                const auto theme = MakeTheme(true,D2D1::ColorF(0.2f,0.4f,0.8f));
                compositor.Dc()->BeginDraw();
                picker.Draw(compositor.Dc(),theme);
                compositor.Dc()->EndDraw();
                const ULONG before = factory->AddRef(); factory->Release();
                compositor.Dc()->BeginDraw();
                for (int i=0;i<1000;++i) picker.Draw(compositor.Dc(),theme);
                compositor.Dc()->EndDraw();
                const ULONG after = factory->AddRef(); factory->Release(); factory->Release();
                ok &= Check(before==after,"1000 Bloom draws retain zero factory references");
            }
        }
        ok &= Check(TestAdvancedSearchDeviceRecovery(host),"advanced search Render recovers injected target loss");
        DestroyWindow(host);
    }
    CoUninitialize();
    return ok;
}
}

int RunUiModuleAuditTest() {
    const auto name = L"PulseUiAudit-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if (!desktop) return Check(false,"create private test desktop") ? 0 : 1;
    bool ok = false;
    std::thread worker([&] { if (SetThreadDesktop(desktop)) ok = RunOnDesktop(); });
    worker.join();
    CloseDesktop(desktop);
    return ok ? 0 : 1;
}
