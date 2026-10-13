#include <windows.h>
#include <shlobj.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <vector>
#include "../fs/fs_net_cache.h"

namespace {
enum class Pause { None, BeforeWrite, BeforeSignal, AfterSignal };
struct Harness {
    std::mutex mutex;
    std::condition_variable changed;
    Pause pause = Pause::None;
    bool entered = false, released = false;
    bool fail_event = false, fail_thread = false;
    BOOL ok = TRUE;
    DWORD elapsed = 0;
    int ticks = 0, closed = 0;
    std::vector<pulse::fs::UncProbeResult> results;
} harness;

void Gate(Pause point) {
    std::unique_lock lock(harness.mutex);
    if (harness.pause != point) return;
    harness.entered = true;
    harness.changed.notify_all();
    harness.changed.wait(lock, [] { return harness.released; });
}
BOOL WINAPI ProbeAttributes(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID) {
    return harness.ok; // No network or user filesystem access.
}
ULONGLONG WINAPI ProbeTick() {
    if (harness.ticks++ == 0) return 100;
    Gate(Pause::BeforeWrite);
    return 100 + harness.elapsed;
}
BOOL WINAPI ProbeSignal(HANDLE event) {
    Gate(Pause::BeforeSignal);
    const BOOL result = ::SetEvent(event);
    Gate(Pause::AfterSignal);
    return result;
}
DWORD WINAPI ProbeWait(HANDLE object, DWORD timeout) {
    if (timeout == 1500) {
        std::unique_lock lock(harness.mutex);
        if (harness.pause != Pause::None && !harness.changed.wait_for(
                lock, std::chrono::seconds(3), [] { return harness.entered; }))
            return WAIT_FAILED;
        lock.unlock();
        return ::WaitForSingleObject(object, 50);
    }
    return ::WaitForSingleObject(object, timeout);
}
BOOL WINAPI ProbePost(HWND, UINT, WPARAM, LPARAM value) {
    auto* result = reinterpret_cast<pulse::fs::UncProbeResult*>(value);
    std::lock_guard lock(harness.mutex);
    harness.results.push_back(*result);
    delete result;
    harness.changed.notify_all();
    return TRUE;
}
BOOL WINAPI ProbeClose(HANDLE object) {
    const BOOL result = ::CloseHandle(object);
    std::lock_guard lock(harness.mutex);
    ++harness.closed;
    harness.changed.notify_all();
    return result;
}
HANDLE WINAPI ProbeEvent(LPSECURITY_ATTRIBUTES a, BOOL b, BOOL c, LPCWSTR d) {
    return harness.fail_event ? nullptr : ::CreateEventW(a, b, c, d);
}
HANDLE WINAPI ProbeThread(LPSECURITY_ATTRIBUTES a, SIZE_T b, LPTHREAD_START_ROUTINE c,
                          LPVOID d, DWORD e, LPDWORD f) {
    return harness.fail_thread ? nullptr : ::CreateThread(a, b, c, d, e, f);
}
}

// Exercise the production owner/worker, real event and real OS thread. Replace
// only the provider, clock, deadline and message sink to force exact interleavings.
#define GetFileAttributesExW ProbeAttributes
#define GetTickCount64 ProbeTick
#define SetEvent ProbeSignal
#define WaitForSingleObject ProbeWait
#define PostMessageW ProbePost
#define CloseHandle ProbeClose
#define CreateEventW ProbeEvent
#define CreateThread ProbeThread
#include "../fs/fs_net_cache.cpp"
#undef GetFileAttributesExW
#undef GetTickCount64
#undef SetEvent
#undef WaitForSingleObject
#undef PostMessageW
#undef CloseHandle
#undef CreateEventW
#undef CreateThread

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
    if (!ok) ++failures;
}
void Reset(Pause pause, DWORD elapsed, BOOL ok = TRUE) {
    std::lock_guard lock(harness.mutex);
    harness.pause = pause;
    harness.elapsed = elapsed;
    harness.ok = ok;
    harness.entered = harness.released = false;
    harness.fail_event = harness.fail_thread = false;
    harness.ticks = harness.closed = 0;
    harness.results.clear();
}
void Run(Pause pause, DWORD elapsed, BOOL ok = TRUE) {
    using namespace pulse::fs;
    Reset(pause, elapsed, ok);
    Check(StartUncProbe(reinterpret_cast<HWND>(1), WM_APP, L"\\\\fixture\\share", 42),
          "production probe starts");
    std::unique_lock lock(harness.mutex);
    const bool posted = harness.changed.wait_for(lock, std::chrono::seconds(3),
        [] { return !harness.results.empty(); });
    Check(posted, "first result arrives while controlled worker is paused");
    const bool timeout = pause == Pause::BeforeWrite || pause == Pause::BeforeSignal;
    if (posted) {
        const auto& first = harness.results.front();
        Check(first.probe_id == 42 && first.unc == L"\\\\fixture\\share", "request identity retained");
        Check(first.status == (timeout || !ok ? NetStatus::Offline :
              elapsed > 800 ? NetStatus::Slow : NetStatus::Online), "first status respects completion and threshold");
        Check(first.rtt_ms == (timeout ? 0 : elapsed), "timeout never publishes worker-private RTT");
        Check(first.final == !timeout, "production result marks only timeout as provisional");
    }
    harness.released = true;
    harness.changed.notify_all();
    const bool finished = harness.changed.wait_for(lock, std::chrono::seconds(3),
        [] { return harness.closed == 2; });
    Check(finished, "worker and event handles close after release");
    if (!finished) std::exit(2); // Do not reuse a fixture still owned by a worker.
    Check(harness.results.size() == (timeout ? 2u : 1u), "only timeout produces a final correction");
    const auto& last = harness.results.back();
    Check(last.final, "completed production probe publishes a final result");
    Check(last.rtt_ms == elapsed && last.status == (!ok ? NetStatus::Offline :
          elapsed > 800 ? NetStatus::Slow : NetStatus::Online), "synchronized final result retains provider RTT and status");
}
}

int main() {
    Run(Pause::BeforeWrite, 799);
    Run(Pause::BeforeSignal, 801);
    Run(Pause::AfterSignal, 800);
    Run(Pause::None, 801);
    Run(Pause::BeforeSignal, 2500, FALSE);
    Reset(Pause::None, 0);
    harness.fail_event = true;
    Check(!pulse::fs::StartUncProbe(reinterpret_cast<HWND>(1), WM_APP, L"fixture", 1),
          "event creation failure is reported");
    harness.fail_event = false;
    harness.fail_thread = true;
    Check(!pulse::fs::StartUncProbe(reinterpret_cast<HWND>(1), WM_APP, L"fixture", 1) && harness.closed == 1,
          "thread creation failure closes event");
    std::cout << "Failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
