#include "../app/single_instance_coordinator.h"
#include <filesystem>
#include <iostream>

int main() {
    using pulse::app::SingleInstanceCoordinator;
    namespace fs = std::filesystem;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; failures += !ok; };
    const auto cwd = fs::current_path();
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"single-instance-" + std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(root / L"sender"); fs::create_directories(root / L"receiver");
    fs::current_path(root / L"sender");
    const auto resolved = SingleInstanceCoordinator::ResolveOpenPath(L"child\\file.txt");
    fs::current_path(root / L"receiver");
    check(resolved && *resolved == (root / L"sender" / L"child" / L"file.txt").wstring(),
          "relative forwarding path is resolved against sender working directory");
    for (const auto* path : {L"", L"pulse:recent", L"shell:Downloads", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"}) {
        check(SingleInstanceCoordinator::ResolveOpenPath(path) == std::optional<std::wstring>(path),
              "empty and virtual navigation tokens remain unchanged");
    }
    fs::current_path(cwd);
    const std::wstring cls = L"Pulse.SingletonAudit." + std::to_wstring(GetCurrentProcessId());
    WNDCLASSW window_class{}; window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = DefWindowProcW; window_class.lpszClassName = cls.c_str();
    if (!RegisterClassW(&window_class)) return 2;
    HWND primary_window = CreateWindowExW(0, cls.c_str(), L"primary", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, window_class.hInstance, nullptr);
    HWND isolated_window = CreateWindowExW(0, cls.c_str(), L"isolated", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, window_class.hInstance, nullptr);
    SingleInstanceCoordinator primary, secondary;
    const auto mutex = L"Local\\Pulse.SingletonAudit." + std::to_wstring(GetCurrentProcessId());
    check(primary.Acquire(mutex) == SingleInstanceCoordinator::AcquireResult::Primary &&
          secondary.Acquire(mutex) == SingleInstanceCoordinator::AcquireResult::Existing,
          "isolated coordinator fixture has one mutex owner");
    check(!secondary.PublishWindow(isolated_window) && primary.PublishWindow(primary_window),
          "only mutex owner publishes primary endpoint");
    check(SingleInstanceCoordinator::FindPrimaryWindow(cls) == primary_window,
          "same-class isolated window cannot intercept forwarded request discovery");
    primary.Release();
    check(!SingleInstanceCoordinator::FindPrimaryWindow(cls), "releasing ownership removes endpoint publication");
    DestroyWindow(isolated_window); DestroyWindow(primary_window);
    UnregisterClassW(cls.c_str(), window_class.hInstance);
    return failures ? 1 : 0;
}
