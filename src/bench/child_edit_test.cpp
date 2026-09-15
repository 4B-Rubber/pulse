#include "../ui/edit_host.h"
#include "../ui/ui_compositor.h"
#include <cstdio>
#include <string>
#include <commctrl.h>
#include <cstdint>
#include <utility>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& compositor = *reinterpret_cast<pulse::ui::Compositor*>(data);
    const auto foreground = D2D1::ColorF(1, 1, 1);
    DWORD flags = 0;
    const bool redirected = GetLayeredWindowAttributes(hwnd, nullptr, nullptr, &flags) && (flags & LWA_ALPHA);
    const auto background = D2D1::ColorF(0, redirected ? 1.0f : 0.0f);
    LRESULT result = 0;
    if (pulse::ui::HandleChildEditMessage(compositor, compositor.TextFormat(), foreground,
        background, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)), hwnd, msg, wp, lp, result)) return result;
    return pulse::ui::DefPresentedChildEditProc(compositor, compositor.TextFormat(), foreground,
        background, hwnd, msg, wp, lp);
}
bool HasRenderedText(pulse::ui::Compositor& compositor, HWND edit) {
    RECT rect{};
    GetClientRect(edit, &rect);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    const auto old = SelectObject(dc, bitmap);
    const bool painted = compositor.PaintLumaEdit(edit, dc, compositor.TextFormat(),
        D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0));
    GdiFlush();
    int ink = 0;
    const auto* data = static_cast<const std::uint32_t*>(pixels);
    if (painted) {
        for (int i = 0; i < rect.right * rect.bottom; ++i)
            if ((data[i] & 0xff) > 64) ++ink;
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return painted && ink > 20;
}
}
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HWND foreground = GetForegroundWindow();
    HWND parent = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", L"1");
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "composition survives a late text engine initialization failure");
        Check(!compositor.LumaTextEnabled(), "partially initialized text engine does not suppress native editing");
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", nullptr);
    }
    {
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "create composition host for native child editors");
        Check(compositor.LumaTextEnabled(), "LumaText remains enabled");
        for (const auto [redirected, scale] : {std::pair{false, 1.0f}, std::pair{false, 1.5f},
                std::pair{true, 1.0f}, std::pair{true, 1.5f}}) {
            compositor.RecreateTextFormats(scale);
            HWND edit = pulse::ui::CreateChildEdit(parent, L"show 中文");
            Check(edit && IsChild(parent, edit) && GetAncestor(edit, GA_ROOT) == parent &&
                !(GetWindowLongPtrW(edit, GWL_STYLE) & WS_POPUP), "editor is a real child, not an owned top-level popup");
            if (!edit) continue;
            if (redirected)
                Check(SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA) != FALSE,
                    "enable redirected surface used by global search");
            SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
            SetWindowPos(edit, nullptr, 20, 30, static_cast<int>(320 * scale), static_cast<int>(30 * scale),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(parent, SW_SHOWNOACTIVATE);
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"show 中文"));
            Check(compositor.PresentLumaEdit(edit, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                D2D1::ColorF(0, redirected ? 1.0f : 0.0f)), "editor presents after native redraw suppression");
            Check(HasRenderedText(compositor, edit), "rendered editor contains visible Unicode glyph pixels");
            RECT before{}, after{};
            GetWindowRect(edit, &before);
            Check(compositor.PresentLumaEdit(edit, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                D2D1::ColorF(0.1f, 0.1f, 0.1f)), "LumaText presents a child bitmap at 100 and 150 percent scale");
            GetWindowRect(edit, &after);
            Check(EqualRect(&before, &after), "text repaint does not move the child into screen coordinates");
            SendMessageW(edit, EM_SETSEL, 0, 4);
            SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"debug"));
            wchar_t text[64]{};
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"debug 中文", "native Unicode editing and selection remain functional");
            SendMessageW(edit, WM_UNDO, 0, 0);
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"show 中文", "native undo remains functional");
            RECT owner{};
            GetWindowRect(parent, &owner);
            SetWindowPos(parent, nullptr, owner.left + 70, owner.top + 40, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetWindowRect(edit, &after);
            Check(after.left == before.left + 70 && after.top == before.top + 40,
                "moving parent moves child without manual repositioning");
            ShowWindow(parent, SW_HIDE);
            Check(!IsWindowVisible(edit), "parent hide automatically hides the editor");
            DestroyWindow(edit);
        }
        HWND edit = pulse::ui::CreateChildEdit(parent);
        DestroyWindow(parent);
        Check(!IsWindow(edit), "destroying parent automatically destroys its editor");
    }
    Check(GetForegroundWindow() == foreground, "tests preserve the user's foreground window");
    CoUninitialize();
    return failures ? 1 : 0;
}
