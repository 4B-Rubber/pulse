#include "../ui/preview_handler_pan.h"
#include <cstdio>
#include <cstdlib>

namespace pulse::ui {
struct PreviewHandlerPanThreadTest {
    static constexpr UINT kMouse = WM_APP + 77;
    struct Context {
        HANDLE queue_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE queue_gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE settled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HWND target = nullptr;
        std::atomic<bool> installed{false};
        std::atomic<unsigned> installs{0}, removes{0}, swallowed{0}, passed{0}, begins{0};
        HOOKPROC callback = nullptr;
        ~Context() { CloseHandle(queue_entered); CloseHandle(queue_gate); CloseHandle(settled); }
    };
    inline static Context* context = nullptr;
    static HHOOK WINAPI Install(int kind, HOOKPROC callback, HINSTANCE, DWORD thread) {
        if (kind != WH_MOUSE_LL || thread != 0) return nullptr;
        context->callback = callback; context->installed = true; ++context->installs;
        return reinterpret_cast<HHOOK>(1);
    }
    static BOOL WINAPI Remove(HHOOK hook) {
        if (hook != reinterpret_cast<HHOOK>(1) || !context->installed.exchange(false)) return FALSE;
        ++context->removes; return TRUE;
    }
    static LRESULT WINAPI Next(HHOOK, int, WPARAM, LPARAM) { return 0; }
    static HWND WINAPI Target(POINT) { return context->target; }
    static BOOL WINAPI Peek(LPMSG message, HWND hwnd, UINT low, UINT high, UINT flags) {
        SetEvent(context->queue_entered);
        if (WaitForSingleObject(context->queue_gate, 3000) != WAIT_OBJECT_0) return FALSE;
        return PeekMessageW(message, hwnd, low, high, flags);
    }
    static BOOL WINAPI Get(LPMSG message, HWND hwnd, UINT low, UINT high) {
        // Reaching the next blocking receive means production reconciliation of
        // the preceding event has completed, including a possible Unhook.
        SetEvent(context->settled);
        const BOOL result = GetMessageW(message, hwnd, low, high);
        if (result > 0 && message->message == kMouse) {
            MSLLHOOKSTRUCT mouse{};
            mouse.pt = {static_cast<SHORT>(LOWORD(message->lParam)), static_cast<SHORT>(HIWORD(message->lParam))};
            bool consumed = false;
            if (context->installed && context->callback)
                consumed = context->callback(HC_ACTION, message->wParam, reinterpret_cast<LPARAM>(&mouse)) != 0;
            if (message->wParam == WM_LBUTTONUP) {
                if (consumed) ++context->swallowed; else ++context->passed;
            }
            if (PreviewHandlerPan::current_input_ && PreviewHandlerPan::current_input_->active)
                ++context->begins;
            message->message = WM_NULL;
        }
        return result;
    }
    static bool Wait(HANDLE event) { return WaitForSingleObject(event, 2500) == WAIT_OBJECT_0; }
    static bool Event(DWORD thread, UINT kind, POINT point) {
        ResetEvent(context->settled);
        return PostThreadMessageW(thread, kMouse, kind, MAKELPARAM(point.x, point.y)) && Wait(context->settled);
    }
    static HANDLE ObserveThread(const std::shared_ptr<PreviewHandlerPan::InputState>& input) {
        HANDLE thread = nullptr;
        DuplicateHandle(GetCurrentProcess(), input->thread, GetCurrentProcess(), &thread, SYNCHRONIZE, FALSE, 0);
        return thread;
    }
    static int Run() {
        int failures = 0;
        auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
        HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Pan private fixture", WS_POPUP,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND child = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 300, 200,
            host, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!host || !child) { if (host) DestroyWindow(host); return 1; }
        ShowWindow(host, SW_SHOWNOACTIVATE);
        POINT down{30,30}; ClientToScreen(child, &down);
        POINT drag{down.x + GetSystemMetrics(SM_CXDRAG) + 20, down.y};
        auto make_pan = [&] {
            auto pan = std::make_unique<PreviewHandlerPan>();
            pan->input_api_ = {Install, Remove, Get, Peek, Next, Target};
            return pan;
        };
        for (int scenario = 0; scenario < 4; ++scenario) {
            Context fixture; context = &fixture; fixture.target = child;
            auto pan = make_pan(); pan->Enable(host);
            check(pan->input_ && Wait(fixture.queue_entered), "real input thread pauses before queue creation");
            if (!pan->input_) continue;
            HANDLE thread = ObserveThread(pan->input_);
            std::weak_ptr<PreviewHandlerPan::InputState> released = pan->input_;
            if (scenario == 0) {
                pan->Disable(); SetEvent(fixture.queue_gate);
                check(Wait(fixture.settled) && fixture.installs == 0, "Enable then Disable before queue creation never installs hook");
                pan.reset();
                check(thread && Wait(thread), "startup cancellation exits real input thread before deadline");
            } else {
                SetEvent(fixture.queue_gate);
                check(Wait(fixture.settled) && fixture.installed && fixture.installs == 1, "enabled input thread installs adapter hook once");
                const DWORD id = pan->input_->thread_id.load();
                check(Event(id, WM_LBUTTONDOWN, down), "press passes through real message loop and registered hook callback");
                if (scenario == 3) {
                    // Pending native click never became an intercepted drag.
                    ResetEvent(fixture.settled); pan->Disable();
                    check(Wait(fixture.settled) && !fixture.installed, "Disable during pending click unhooks without owning its release");
                    ResetEvent(fixture.settled); pan->Enable(host); check(Wait(fixture.settled), "re-enable wakes same input thread");
                    const auto begins = fixture.begins.load();
                    check(Event(id, WM_MOUSEMOVE, drag) && fixture.begins == begins, "invalid pending press cannot take over movement after re-enable");
                    check(Event(id, WM_LBUTTONUP, drag) && fixture.swallowed == 0, "pending native click release remains unconsumed");
                    pan.reset(); check(thread && Wait(thread), "pending cancellation thread exits before deadline");
                } else {
                    check(Event(id, WM_MOUSEMOVE, drag) && fixture.begins > 0, "threshold crossing starts real input-thread drag takeover");
                    if (scenario == 1) {
                        ResetEvent(fixture.settled); pan->Disable();
                        check(Wait(fixture.settled) && fixture.installed && fixture.removes == 0, "Disable retains hook until owned release arrives");
                        const auto begins = fixture.begins.load();
                        check(Event(id, WM_LBUTTONDOWN, down) && Event(id, WM_MOUSEMOVE, drag) && fixture.begins == begins,
                              "disabled release-only state cannot take over another gesture");
                        check(Event(id, WM_LBUTTONUP, drag) && fixture.swallowed == 1 && !fixture.installed && fixture.removes == 1,
                              "matching release consumed once before hook removal");
                        check(Event(id, WM_LBUTTONUP, drag) && fixture.swallowed == 1 && fixture.passed == 1,
                              "later release is not consumed after hook removed");
                        pan.reset(); check(thread && Wait(thread), "disabled thread stops promptly after destructor");
                    } else {
                        ResetEvent(fixture.settled); pan.reset();
                        check(Wait(fixture.settled) && fixture.installed && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT,
                              "destructor stop retains input thread and hook for paired release");
                        check(PostThreadMessageW(id, kMouse, WM_LBUTTONUP, MAKELPARAM(drag.x, drag.y)) && thread && Wait(thread),
                              "paired release allows stopped real message-loop thread to exit");
                        check(fixture.swallowed == 1 && fixture.removes == 1 && !fixture.installed,
                              "destructor path consumes release once and unhooks exactly once");
                    }
                }
            }
            if (thread && WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) {
                // Failure cleanup only: never let an adapter thread outlive its
                // private context. A stuck case remains a failed test.
                check(false, "input thread missed exit deadline");
                if (const auto state = released.lock()) {
                    SetEvent(fixture.queue_gate);
                    PostThreadMessageW(state->thread_id, WM_QUIT, 0, 0);
                }
                if (!Wait(thread)) std::quick_exit(2);
            }
            check(released.expired(), "thread shared state and original thread handle released after exit");
            if (thread) CloseHandle(thread);
            // No callback can survive the completed thread or refer to this context.
        }
        context = nullptr; DestroyWindow(host);
        std::printf("Failures: %d\n", failures); return failures;
    }
};
}
int wmain() { return pulse::ui::PreviewHandlerPanThreadTest::Run() ? 1 : 0; }
