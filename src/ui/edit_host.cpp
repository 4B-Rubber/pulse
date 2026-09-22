#include "edit_host.h"
#include <commctrl.h>
#include <cstdio>
#include <imm.h>
#pragma comment(lib, "imm32.lib")
namespace pulse::ui {
constexpr UINT_PTR kEditCaretTimer = 71;
namespace {
constexpr wchar_t kNativeEdit[] = L"Pulse.NativeEditFallback";
// This edit hides its caret (the presented bitmap draws its own), so an input method that asks
// the system where the caret is - the TSF bridge does, to put the insertion point back after it
// commits a composition - is told "position 0", and the next character lands in front of
// everything typed: "demo" then "1" produced "1demo" in the inline rename box. The guard below
// remembers where a burst of committed characters left the caret and drops a request that moves
// it behind that point. A click, a navigation key or a focus change ends the burst.
constexpr wchar_t kImeCaretBurst[] = L"Pulse.ImeCaretBurst";
constexpr ULONGLONG kImeBurstWindowMs = 2000;
constexpr unsigned long long kCaretLogLimitBytes = 256ull * 1024ull;
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
// Appends one line to a log file, rotating it once it grows past `limit` (0 keeps everything).
void AppendLog(const std::wstring& path, const wchar_t* line, unsigned long long limit = 0) {
    if (limit != 0) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
            ((static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) > limit) {
            DeleteFileW(path.c_str());
        }
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") == 0 && file) {
        fwprintf(file, L"%s\n", line);
        fclose(file);
    }
}
// What the guard decided, next to the app's other logs: on a machine whose input method cannot be
// reproduced here, this says whether the guard ran and which request it saw.
void LogCaretGuard(const wchar_t* decision, int from, int to, DWORD caret_after) {
    static const std::wstring path = [] {
        wchar_t local[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local)) == 0)
            return std::wstring{};
        const std::wstring dir = std::wstring(local) + L"\\Pulse";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\ime_caret.log";
    }();
    if (path.empty()) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[160]{};
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s sel=%d,%d caret=%lu",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, decision, from, to, caret_after);
    AppendLog(path, line, kCaretLogLimitBytes);
}
// True when the request is the input method's own idea of where the caret goes and must not land.
bool DropImeCaretRequest(HWND hwnd, ImeCaretBurst* burst, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!burst || msg != EM_SETSEL || burst->tick == 0 || GetFocus() != hwnd ||
        GetTickCount64() - burst->tick > kImeBurstWindowMs) {
        return false;
    }
    const int from = static_cast<int>(wParam);
    const int to = static_cast<int>(lParam);
    const bool drop = from >= 0 && to >= 0 && to < static_cast<int>(burst->caret_after_commit);
    LogCaretGuard(drop ? L"drop" : L"keep", from, to, burst->caret_after_commit);
    return drop;
}
DWORD ReadSelection(HWND hwnd, DWORD* start = nullptr) {
    DWORD sel[2]{};
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&sel[0]),
                 reinterpret_cast<LPARAM>(&sel[1]));
    if (start) *start = sel[0];
    return sel[1];
}
// Where the caret belongs after a text that an input method rewrote: just past the changed span.
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
bool CustomEdit(Compositor& compositor, HWND hwnd) {
    return compositor.LumaTextEnabled() && !GetPropW(hwnd, kNativeEdit);
}
void PresentEdit(Compositor& compositor, HWND hwnd, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background) {
    if (compositor.PresentLumaEdit(hwnd, format, foreground, background)) return;
    // Keep native EDIT input, selection and IME together if presentation fails.
    SetPropW(hwnd, kNativeEdit, reinterpret_cast<HANDLE>(1));
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    KillTimer(hwnd, kEditCaretTimer);
    if (GetFocus() == hwnd) ShowCaret(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}
}
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    (void)background_brush;
    if (msg == WM_NCDESTROY) RemovePropW(hwnd, kNativeEdit);
    // A click or a navigation key is the user taking the caret back: it ends the input method
    // burst before anything on this layer can be mistaken for one (see kImeCaretBurst).
    if (EndsImeCaretBurst(msg, wParam)) EndImeCaretBurst(hwnd);
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
    // PULSE_EDIT_TRACE=<file>: record what a hosted edit receives, with the text and selection
    // around every message, plus the caret guard's state - when a reset is not dropped the log has
    // to say whether the guard was still armed, otherwise the reason stays a guess. This is how a
    // report ("typing through the IME reorders the text") turns into a message sequence.
    const auto trace = [&](const wchar_t* phase) {
        static const std::wstring path = [] {
            wchar_t value[512]{};
            if (GetEnvironmentVariableW(L"PULSE_EDIT_TRACE", value, ARRAYSIZE(value)) == 0)
                return std::wstring{};
            return std::wstring(value);
        }();
        if (path.empty()) return;
        wchar_t text[256]{};
        GetWindowTextW(hwnd, text, ARRAYSIZE(text));
        wchar_t composing[128]{};
        if (HIMC context = ImmGetContext(hwnd)) {
            const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, composing,
                                                        static_cast<DWORD>(sizeof(composing)));
            if (bytes > 0 && bytes / 2 < static_cast<LONG>(ARRAYSIZE(composing)))
                composing[bytes / 2] = L'\0';
            ImmReleaseContext(hwnd, context);
        }
        const ImeCaretBurst* burst = ImeBurst(hwnd, false);
        const bool armed = burst && burst->tick != 0 &&
            GetTickCount64() - burst->tick <= kImeBurstWindowMs;
        DWORD sel_start = 0;
        const DWORD sel_end = ReadSelection(hwnd, &sel_start);
        wchar_t line[512]{};
        swprintf_s(line, L"%s msg=0x%04X w=%08zX l=%08zX sel=%lu,%lu burst=%s caret=%lu text=[%s] comp=[%s]",
            phase, msg, static_cast<size_t>(wParam), static_cast<size_t>(lParam),
            sel_start, sel_end, armed ? L"armed" : L"-",
            burst ? burst->caret_after_commit : 0, text, composing);
        AppendLog(path, line);
    };
    const bool trace_msg = msg == WM_SETTEXT || msg == WM_CHAR || msg == WM_KEYDOWN ||
        msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_ENDCOMPOSITION || msg == WM_IME_CHAR || msg == WM_IME_SETCONTEXT ||
        msg == EM_SETSEL;
    if (trace_msg) trace(L"before");
    if (msg == WM_NCDESTROY) DiscardImeCaretBurst(hwnd);
    // See kImeCaretBurst for why a burst has to be remembered and what ends it.
    const bool ime_typing = msg == WM_CHAR || msg == WM_IME_CHAR ||
        msg == WM_IME_COMPOSITION || msg == WM_IME_ENDCOMPOSITION;
    ImeCaretBurst* burst = ImeBurst(hwnd, ime_typing || msg == EM_SETSEL);
    // Mouse messages are consumed by HandleChildEditMessage before they reach this procedure, so
    // the burst ends there as well; see the call at its top.
    if (EndsImeCaretBurst(msg, wParam)) EndImeCaretBurst(hwnd);
    if (DropImeCaretRequest(hwnd, burst, msg, wParam, lParam)) {
        if (trace_msg) trace(L"guard-drop");
        return 0;
    }
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
    const bool read_before = msg == WM_SETTEXT && custom_paint;
    std::wstring text_before;
    DWORD start_before = 0;
    DWORD end_before = 0;
    if (read_before) {
        text_before = EditText(hwnd);
        end_before = ReadSelection(hwnd, &start_before);
    }
    if (custom_paint) SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (burst && ime_typing) {
        burst->caret_after_commit = ReadSelection(hwnd);
        burst->tick = GetTickCount64();
        // One line per run, so a log that contains nothing but this still says the guard was live.
        static bool announced = false;
        if (!announced) {
            announced = true;
            LogCaretGuard(L"armed-first", 0, 0, burst->caret_after_commit);
        }
    }
    if (read_before) {
        const std::wstring text_after = EditText(hwnd);
        if (text_after == text_before) {
            SendMessageW(hwnd, EM_SETSEL, start_before, end_before);
        } else {
            const size_t caret = CaretPastRewrite(text_before, text_after);
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(caret), static_cast<LPARAM>(caret));
        }
    }
    if (custom_paint) {
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        HideCaret(hwnd);
        PresentEdit(compositor, hwnd, format,
            foreground, background);
    }
    if (trace_msg) trace(L"after");
    return result;
}

}
