#include "../app/unc_probe_scheduler.h"
#include <iostream>
#include <limits>
#include <string>

namespace {
using pulse::app::UncProbeScheduler;
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++failures;
}
uint64_t Begin(UncProbeScheduler& scheduler, const std::wstring& path) {
    return scheduler.Begin(path);
}
bool Apply(UncProbeScheduler& scheduler, const std::wstring& path, uint64_t id,
           bool final, bool online, bool& readonly) {
    if (!scheduler.AcceptResult(path, id, final)) return false;
    scheduler.Finish(id);
    readonly = !online;
    return true;
}
void Reordered(bool new_online) {
    UncProbeScheduler scheduler;
    bool readonly = false;
    const auto a = Begin(scheduler, L"a");
    Check(Apply(scheduler, L"a", a, false, false, readonly) && readonly && scheduler.active_id == 0,
          "timeout applies offline and releases the scheduling slot");
    const auto b = Begin(scheduler, L"a");
    Check(!Apply(scheduler, L"a", a, true, !new_online, readonly) && scheduler.IsActive(b),
          "late A cannot update state or release active B");
    Check(Apply(scheduler, L"a", b, true, new_online, readonly) && readonly == !new_online,
          "B applies its own result");
    Check(!Apply(scheduler, L"a", a, true, !new_online, readonly) && readonly == !new_online,
          "late A after B completion cannot reverse newer connection state");
}
}
int main() {
    Reordered(false);
    Reordered(true);
    UncProbeScheduler scheduler;
    bool readonly = false;
    const auto a = Begin(scheduler, L"a");
    Apply(scheduler, L"a", a, false, false, readonly);
    const auto b = Begin(scheduler, L"b");
    Check(Apply(scheduler, L"a", a, true, true, readonly) && !readonly && scheduler.IsActive(b),
          "latest A may recover from timeout while a different path B owns the slot");
    Check(!Apply(scheduler, L"wrong", b, true, true, readonly) && scheduler.IsActive(b),
          "matching ID with wrong path cannot change state or release the slot");
    Check(Apply(scheduler, L"b", b, true, false, readonly) && readonly,
          "B remains consumable after a rejected wrong-path result");
    Check(!Apply(scheduler, L"b", b, true, true, readonly) && readonly,
          "duplicate final result is ignored");
    Check(!Apply(scheduler, L"b", 0, true, true, readonly) && readonly,
          "zero probe ID cannot update state");
    uint64_t first = Begin(scheduler, L"rapid");
    uint64_t last = first;
    for (int i = 0; i < 1000; ++i) last = Begin(scheduler, L"rapid");
    Check(!Apply(scheduler, L"rapid", first, true, true, readonly) && scheduler.IsActive(last),
          "rapid replacement rejects oldest request without losing current ownership");
    Check(Apply(scheduler, L"rapid", last, true, true, readonly) && !readonly,
          "rapid replacement accepts the latest request");
    scheduler.next_id = (std::numeric_limits<uint64_t>::max)();
    const auto max = Begin(scheduler, L"wrap-a");
    const auto wrapped = Begin(scheduler, L"wrap-b");
    Check(max != 0 && wrapped != 0 && max != wrapped && scheduler.IsActive(wrapped),
          "request counter skips zero on wrap");
    const auto failed = Begin(scheduler, L"failed-start");
    scheduler.Cancel(L"failed-start", failed);
    Check(scheduler.active_id == 0 && !Apply(scheduler, L"failed-start", failed, true, true, readonly),
          "start failure relinquishes its slot and result ownership");
    const auto replacement = Begin(scheduler, L"failed-start");
    scheduler.Cancel(L"failed-start", failed);
    Check(scheduler.IsActive(replacement) && Apply(scheduler, L"failed-start", replacement, true, false, readonly),
          "cancelling an obsolete request preserves its replacement");
    const auto timeout = Begin(scheduler, L"closing-a");
    Apply(scheduler, L"closing-a", timeout, false, false, readonly);
    const auto active = Begin(scheduler, L"closing-b");
    scheduler.Clear();
    Check(scheduler.active_id == 0 && !Apply(scheduler, L"closing-a", timeout, true, true, readonly) &&
          !Apply(scheduler, L"closing-b", active, true, true, readonly) && readonly,
          "shutdown discards both timed-out and active results without updating state");
    const auto restarted = Begin(scheduler, L"closing-b");
    Check(restarted != active && !Apply(scheduler, L"closing-b", active, true, true, readonly) &&
          scheduler.IsActive(restarted), "reset does not reuse a retired request ID");
    Check(Apply(scheduler, L"closing-b", restarted, false, false, readonly) &&
          Apply(scheduler, L"closing-b", restarted, true, true, readonly) && !readonly,
          "same-ID final completion still corrects its provisional timeout");
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
