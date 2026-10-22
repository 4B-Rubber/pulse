#include "edit_host.h"
#include "typography.h"
#include <commctrl.h>
#include <algorithm>
#include <cstdio>
#include <string>
namespace pulse::ui {
constexpr UINT_PTR kEditCaretTimer = 71;
namespace {
constexpr wchar_t kNativeEdit[] = L"Pulse.NativeEditFallback";
constexpr wchar_t kPolicyNative[] = L"Pulse.NativeEditPolicy";
constexpr wchar_t kRestoreUploaded[] = L"Pulse.EditRestoreUploaded";
constexpr wchar_t kMigrating[] = L"Pulse.EditMigrating";
// This edit hides its caret (the presented bitmap draws its own), so an input method that asks
// the system where the caret is - the TSF bridge does, to put the insertion point back after it
// commits a composition - is told "position 0", and the next character lands in front of
// everything typed: "demo" then "1" produced "1demo" in the inline rename box. The guard below
// remembers where a burst of committed characters left the caret and drops a request that moves
// it behind that point. A click, a navigation key or a focus change ends the burst.
constexpr wchar_t kImeCaretBurst[] = L"Pulse.ImeCaretBurst";
constexpr ULONGLONG kImeBurstWindowMs = 2000;
struct ImeCaretBurst {
    DWORD caret_after_commit = 0;
    ULONGLONG tick = 0;
};
ImeCaretBurst* ImeBurst(HWND hwnd, bool create) {
    auto* burst = static_cast<ImeCaretBurst*>(GetPropW(hwnd, kImeCaretBurst));
    if (!burst && create) {
        burst = new ImeCaretBurst;
        SetPropW(hwnd, kImeCaretBurst, burst);
    }
    return burst;
}
void EndImeCaretBurst(HWND hwnd) {
    if (auto* burst = ImeBurst(hwnd, false)) {
        burst->tick = 0;
        burst->caret_after_commit = 0;
    }
}
void DiscardImeCaretBurst(HWND hwnd) {
    if (auto* burst = ImeBurst(hwnd, false)) {
        RemovePropW(hwnd, kImeCaretBurst);
        delete burst;
    }
}
// Only the keys that move the caret on their own end the burst. Ordinary characters do not: an
// input method hands its keys through to the edit as plain key-downs while it commits, and
// treating those as navigation threw the guard away exactly when a fast typist needed it. A mouse
// move does not end it either - only the click that follows does.
bool EndsImeCaretBurst(UINT msg, WPARAM wParam) {
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_SETFOCUS ||
        msg == WM_KILLFOCUS) {
        return true;
    }
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;
    switch (wParam) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_DELETE: case VK_BACK: case VK_INSERT:
        return true;
    default:
        return false;
    }
}
DWORD ReadSelection(HWND hwnd, DWORD* start = nullptr) {
    DWORD sel[2]{};
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&sel[0]),
                 reinterpret_cast<LPARAM>(&sel[1]));
    if (start) *start = sel[0];
    return sel[1];
}
// Where the caret belongs after a text an input method rewrote: just past the changed span.
size_t CaretPastRewrite(const std::wstring& before, const std::wstring& after) {
    size_t prefix = 0;
    while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix])
        ++prefix;
    size_t suffix = 0;
    while (suffix + prefix < before.size() && suffix + prefix < after.size() &&
           before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
        ++suffix;
    }
    return after.size() - suffix;
}
std::wstring EditText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}
// The caret this layer puts back after a rewritten text must not look like the input method's own
// idea of where it goes (see DropImeCaretRequest).
thread_local bool g_restoring_caret = false;
// Guard decisions go to the PULSE_EDIT_TRACE file: on a machine whose input method cannot be
// reproduced here, the log then says whether the guard was live and which request it saw.
void LogCaretGuard(const wchar_t* decision, int from, int to, DWORD caret_after) {
    static const std::wstring path = [] {
        wchar_t value[512]{};
        if (GetEnvironmentVariableW(L"PULSE_EDIT_TRACE", value, ARRAYSIZE(value)) == 0)
            return std::wstring{};
        return std::wstring(value);
    }();
    if (path.empty()) return;
    wchar_t line[160]{};
    swprintf_s(line, L"guard %s sel=%d,%d caret=%lu\n", decision, from, to, caret_after);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") == 0 && file) {
        fwprintf(file, L"%s", line);
        fclose(file);
    }
}
// True when the request is the input method's own idea of where the caret goes and must not land.
bool DropImeCaretRequest(HWND hwnd, ImeCaretBurst* burst, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_restoring_caret || !burst || msg != EM_SETSEL || burst->tick == 0 || GetFocus() != hwnd ||
        GetTickCount64() - burst->tick > kImeBurstWindowMs) {
        return false;
    }
    const int from = static_cast<int>(wParam);
    const int to = static_cast<int>(lParam);
    const bool drop = from >= 0 && to >= 0 && to < static_cast<int>(burst->caret_after_commit);
    LogCaretGuard(drop ? L"drop" : L"keep", from, to, burst->caret_after_commit);
    return drop;
}
void RestoreNativeCaret(HWND hwnd) {
    if (GetFocus() != hwnd) return;
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == hwnd) {
        // Custom mouse/paint paths can hide the system caret more than once.
        // Recreate it at the EDIT's current position to reset that hide count.
        const int width = (std::max)(1L, info.rcCaret.right - info.rcCaret.left);
        const int height = (std::max)(1L, info.rcCaret.bottom - info.rcCaret.top);
        if (CreateCaret(hwnd, nullptr, width, height)) SetCaretPos(info.rcCaret.left, info.rcCaret.top);
    }
    ShowCaret(hwnd);
}
bool CustomEdit(Compositor& compositor, HWND hwnd) {
    const bool custom = compositor.CustomEditEnabled() && !GetPropW(hwnd, kNativeEdit);
    if (GetPropW(hwnd, kMigrating)) return custom;
    const bool policy_native = GetPropW(hwnd, kPolicyNative) != nullptr;
    if (!custom && !policy_native) {
        SetPropW(hwnd, kMigrating, reinterpret_cast<HANDLE>(1));
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(hwnd, nullptr, nullptr, &flags) || !(flags & LWA_ALPHA))
            SetPropW(hwnd, kRestoreUploaded, reinterpret_cast<HANDLE>(1));
        SetPropW(hwnd, kPolicyNative, reinterpret_cast<HANDLE>(1));
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        KillTimer(hwnd, kEditCaretTimer);
        RestoreNativeCaret(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        RemovePropW(hwnd, kMigrating);
    } else if (custom && policy_native) {
        SetPropW(hwnd, kMigrating, reinterpret_cast<HANDLE>(1));
        if (RemovePropW(hwnd, kRestoreUploaded)) {
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
        }
        RemovePropW(hwnd, kPolicyNative);
        if (GetFocus() == hwnd) {
            HideCaret(hwnd);
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        RemovePropW(hwnd, kMigrating);
    }
    return custom;
}
bool PresentEdit(Compositor& compositor, HWND hwnd, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background) {
    bool force_failure = false;
#ifdef PULSE_WITH_SELFTEST
    force_failure = GetPropW(hwnd, L"Pulse.Test.LumaPresentFailure") != nullptr;
#endif
    if (!force_failure && compositor.PresentLumaEdit(hwnd, format, foreground, background)) return true;
    // Keep native EDIT input, selection and IME together if presentation fails.
    SetPropW(hwnd, kNativeEdit, reinterpret_cast<HANDLE>(1));
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    KillTimer(hwnd, kEditCaretTimer);
    RestoreNativeCaret(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    return false;
}
}
bool SynchronizeChildEditBackend(Compositor& compositor, HWND hwnd) {
    return hwnd && CustomEdit(compositor, hwnd);
}
bool PresentChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd) {
    if (!hwnd || !CustomEdit(compositor, hwnd)) return false;
    HideCaret(hwnd);
    return PresentEdit(compositor, hwnd, format, foreground, background);
}
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    (void)background_brush;
    // A click or a navigation key is the user taking the caret back: it ends the input method
    // burst before anything on this layer can be mistaken for one (see kImeCaretBurst).
    if (EndsImeCaretBurst(msg, wParam)) EndImeCaretBurst(hwnd);
    if (msg == WM_NCDESTROY) {
        compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
        RemovePropW(hwnd, kNativeEdit);
        RemovePropW(hwnd, kPolicyNative);
        RemovePropW(hwnd, kRestoreUploaded);
        RemovePropW(hwnd, kMigrating);
        return false;
    }
    if (msg == typography::TextBackendChangedMessage()) {
        if (CustomEdit(compositor, hwnd) && IsWindowVisible(hwnd))
            PresentEdit(compositor, hwnd, format, foreground, background);
        result = 0;
        return true;
    }
    if (CustomEdit(compositor, hwnd) &&
        (msg == WM_PRINT || msg == WM_PRINTCLIENT || msg == WM_NCPAINT)) {
        result = 0;
        return true;
    }
    if (CustomEdit(compositor, hwnd) && msg == WM_ERASEBKGND) {
        result = 1;
        return true;
    }
    switch (msg) {
    case WM_LBUTTONDOWN: {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (!CustomEdit(compositor, hwnd)) return false;
        result = compositor.CallLumaEditMouse(
            hwnd, msg, wParam, lParam, format);
        if (msg != WM_MOUSEMOVE || GetCapture() == hwnd) {
            PresentEdit(compositor, hwnd, format,
                                         foreground, background);
        }
        return true;
    }
    case WM_PAINT: {
        if (!CustomEdit(compositor, hwnd)) return false;
        HideCaret(hwnd);
        PresentEdit(compositor, hwnd, format, foreground, background);
        result = 0;
        return true;
    }
    case WM_SETFOCUS: {
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (CustomEdit(compositor, hwnd)) {
            HideCaret(hwnd);
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
            PresentEdit(compositor, hwnd, format,
                                         foreground, background);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }
    case WM_KILLFOCUS:
        compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
        KillTimer(hwnd, kEditCaretTimer);
        return false;
    case WM_TIMER:
        if (wParam == kEditCaretTimer) {
            if (GetCapture() != hwnd && CustomEdit(compositor, hwnd)) {
                PresentEdit(compositor, hwnd, format,
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
    if (msg == WM_IME_STARTCOMPOSITION) {
        // Start every composition from a caret, not a selection. When a word
        // replaced the selected stem (F2 selects it), Microsoft Pinyin then
        // moved the caret back to the start of the field, so the next word
        // went in front of the first (#116, #162, #211). Deleting the
        // selection first is what the result would do anyway.
        DWORD from = 0, to = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
        if (from != to) SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    }
    if (msg == WM_NCDESTROY) DiscardImeCaretBurst(hwnd);
    // See kImeCaretBurst for why a burst has to be remembered and what ends it.
    const bool ime_typing = msg == WM_CHAR || msg == WM_IME_CHAR ||
        msg == WM_IME_COMPOSITION || msg == WM_IME_ENDCOMPOSITION;
    ImeCaretBurst* burst = ImeBurst(hwnd, ime_typing || msg == EM_SETSEL);
    // Mouse messages are consumed by HandleChildEditMessage before they reach this procedure, so
    // the burst ends there as well; see the call at its top.
    if (EndsImeCaretBurst(msg, wParam)) EndImeCaretBurst(hwnd);
    if (DropImeCaretRequest(hwnd, burst, msg, wParam, lParam)) return 0;
    const bool changes_visual = msg == WM_SETTEXT || msg == EM_SETSEL || msg == EM_REPLACESEL ||
        msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_CUT || msg == WM_PASTE ||
        msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_ENDCOMPOSITION || msg == WM_SETFONT || msg == WM_SIZE || msg == EM_SETMARGINS ||
        msg == EM_SETCUEBANNER;
    const bool custom_paint = changes_visual && CustomEdit(compositor, hwnd) && IsWindowVisible(hwnd);
    // An EDIT puts the caret back to the front on every WM_SETTEXT, even when the text it is given
    // is the one it already has: an input method that commits by rewriting the whole text dropped
    // the caret at 0 behind the user's back and the next character landed in front of everything.
    // Keep the caret where the user was - the same selection for a redundant write, and just past
    // the changed span for a rewritten one, which is where the composition the IME wrote ends.
    // Independent of how the edit is painted: the native fallback resets the caret the same way.
    const bool keep_caret = msg == WM_SETTEXT;
    std::wstring text_before;
    DWORD start_before = 0, end_before = 0;
    if (keep_caret) {
        text_before = EditText(hwnd);
        end_before = ReadSelection(hwnd, &start_before);
    }
    if (custom_paint) SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (burst && ime_typing) {
        burst->caret_after_commit = ReadSelection(hwnd);
        burst->tick = GetTickCount64();
        // One line per run, so a log that contains nothing else still says the guard was live.
        static bool announced = false;
        if (!announced) {
            announced = true;
            LogCaretGuard(L"armed-first", 0, 0, burst->caret_after_commit);
        }
    }
    if (keep_caret) {
        const std::wstring text_after = EditText(hwnd);
        g_restoring_caret = true;
        if (text_after == text_before) {
            SendMessageW(hwnd, EM_SETSEL, start_before, end_before);
        } else {
            const size_t caret = CaretPastRewrite(text_before, text_after);
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(caret), static_cast<LPARAM>(caret));
        }
        g_restoring_caret = false;
    }
    if (changes_visual) compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
    if (custom_paint) {
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        HideCaret(hwnd);
        PresentEdit(compositor, hwnd, format,
            foreground, background);
    }
    return result;
}

}
