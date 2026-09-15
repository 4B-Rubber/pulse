#include "edit_host.h"
#include <commctrl.h>
namespace pulse::ui {
constexpr UINT_PTR kEditCaretTimer = 71;
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    if (compositor.LumaTextEnabled() &&
        (msg == WM_PRINT || msg == WM_PRINTCLIENT || msg == WM_NCPAINT)) {
        result = 0;
        return true;
    }
    if (compositor.LumaTextEnabled() && msg == WM_ERASEBKGND) {
        result = 1;
        return true;
    }
    switch (msg) {
    case WM_LBUTTONDOWN: {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (!compositor.LumaTextEnabled()) return false;
        result = compositor.CallLumaEditMouse(
            hwnd, msg, wParam, lParam, format);
        if (msg != WM_MOUSEMOVE || GetCapture() == hwnd) {
            compositor.PresentLumaEdit(hwnd, format,
                                         foreground, background);
        }
        return true;
    }
    case WM_PAINT: {
        if (!compositor.LumaTextEnabled()) return false;
        HideCaret(hwnd);
        if (!compositor.PresentLumaEdit(hwnd, format,
                                          foreground, background)) {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);

            FillRect(hdc, &ps.rcPaint, background_brush);
            EndPaint(hwnd, &ps);
        }
        result = 0;
        return true;
    }
    case WM_SETFOCUS: {
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (compositor.LumaTextEnabled()) {
            HideCaret(hwnd);
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
            compositor.PresentLumaEdit(hwnd, format,
                                         foreground, background);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }
    case WM_KILLFOCUS:
        KillTimer(hwnd, kEditCaretTimer);
        return false;
    case WM_TIMER:
        if (wParam == kEditCaretTimer) {
            if (GetCapture() != hwnd && compositor.LumaTextEnabled()) {
                compositor.PresentLumaEdit(hwnd, format,
                                             foreground, background);
            } else if (GetCapture() != hwnd) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            result = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}

LRESULT DefPresentedChildEditProc(Compositor& compositor, IDWriteTextFormat* format, D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool changes_visual = msg == WM_SETTEXT || msg == EM_SETSEL || msg == EM_REPLACESEL ||
        msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_CUT || msg == WM_PASTE ||
        msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_ENDCOMPOSITION || msg == WM_SETFONT || msg == WM_SIZE || msg == EM_SETMARGINS ||
        msg == EM_SETCUEBANNER;
    const bool custom_paint = changes_visual && compositor.LumaTextEnabled() && IsWindowVisible(hwnd);
    if (custom_paint) SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (custom_paint) {
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        HideCaret(hwnd);
        compositor.PresentLumaEdit(hwnd, format,
            foreground, background);
    }
    return result;
}

}
