#include "startup_rollback_test.h"
#include "../app/app_runtime.h"
#include "../common/utf8_file.h"
#include "../common/localization.h"
#include "../common/crash_reporter.h"
#include <filesystem>
#include <cstdio>
#include <map>
#include <fstream>

namespace {
int fault_stage = 0;
int failures = 0;
WNDPROC close_procedure = nullptr;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    failures += !ok;
}
}
bool StartupRollbackFaultForTest(int stage) { return fault_stage == stage; }

int RunStartupRollbackTest(HINSTANCE instance, WNDPROC window_proc) {
    using namespace pulse;
    const auto root = std::filesystem::absolute(L"bench_data/startup-rollback-" +
        std::to_wstring(GetCurrentProcessId()));
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
    const auto temporary = root / L"temp";
    std::filesystem::create_directories(temporary);
    SetEnvironmentVariableW(L"TEMP", temporary.c_str());
    SetEnvironmentVariableW(L"TMP", temporary.c_str());
    crash::Initialize({crash::ProcessRole::App, false, root.wstring()});
    const auto desktop_name = L"PulseStartupAudit_" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(instance, L"en-US");
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"PulseStartupAudit";
    if (!RegisterClassW(&cls)) return 2;
    const std::map<std::wstring, std::wstring> originals{
        {L"app.json", L"{\"language\":\"en-US\",\"show_status_performance\":true}"},
        {L"context_menu.json", L"{\"share\":true,\"items\":{\"fixture\":false}}"},
        {L"places.json", L"{\"starred\":[\"C:\\\\kept-fixture\"],\"network\":[]}"},
        {L"session.json", L"{\"version\":3,\"active_path\":\"C:\\\\kept-fixture\",\"undo\":{\"fixture\":1}}"}
    };
    for (const auto& [name, text] : originals)
        Check(WriteUtf8FileAtomic((root / name).wstring(), text), "write isolated nondefault startup fixture");
    const auto read_bytes = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    std::map<std::wstring, std::string> original_bytes;
    for (const auto& [name, text] : originals) original_bytes[name] = read_bytes(root / name);
    for (int stage : {1, 2}) {
        fault_stage = stage;
        {
            auto state = std::make_unique<AppState>();
            // The saved session is intentionally still transitional when startup fails.
            state->session_path = L"C:\\kept-fixture";
            state->pending_undo_json = L"{\"fixture\":1}";
            state->session_layout_tabs.resize(2);
            HWND window = CreateWindowExW(0, cls.lpszClassName, L"startup rollback",
                WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr, nullptr, instance, state.get());
            Check(!window && !state->hwnd && !state->startupComplete,
                  "actual WM_CREATE failure destroys window without enabling persistence");
            Check(!PrepareSessionForUpdate(*state), "update handshake refuses uninitialized state");
            if (window) DestroyWindow(window);
        }
        for (const auto& [name, text] : originals) {
            Check(read_bytes(root / name) == original_bytes.at(name),
                  "rollback and state destruction preserve configuration bytes");
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    }
    fault_stage = 0;
    {
        auto state = std::make_unique<AppState>();
        state->window_tabs.EnsureDefault();
        BindCurrentLayout(*state);
        state->pane->NewTab(root.wstring());
        state->startupComplete = true;
        state->appPrefs.language = L"en-US";
        Check(PrepareSessionForUpdate(*state), "ready models still support update handshake persistence");
        app::SessionSnapshot session;
        Check(app::LoadSession(session) && session.active_path == fs::NormalizePath(root.wstring()) &&
            session.layout_tabs.size() == 1, "ready session persists live layout and path");
        state->appPrefs.load_failed = true;
        Check(!PrepareSessionForUpdate(*state), "ready state retains damaged preferences write protection");
        state->appPrefs.load_failed = false;
        state->updateSessionPrepared = false;
        state->appPrefs.language = L"zh-CN";
        state->pane->NewTab((root / L"closed").wstring());
        close_procedure = window_proc;
        WNDCLASSW ready_class{};
        ready_class.hInstance = instance;
        ready_class.lpszClassName = L"PulseReadyCloseAudit";
        ready_class.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
            if (message == WM_NCCREATE) {
                auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            }
            if (message == WM_DESTROY) return close_procedure(hwnd, message, wparam, lparam);
            return DefWindowProcW(hwnd, message, wparam, lparam);
        };
        RegisterClassW(&ready_class);
        state->hwnd = CreateWindowExW(0, ready_class.lpszClassName, L"ready close", WS_POPUP,
            0, 0, 200, 100, nullptr, nullptr, instance, state.get());
        Check(state->hwnd && DestroyWindow(state->hwnd) && app::LoadSession(session) &&
            session.active_path == fs::NormalizePath((root / L"closed").wstring()),
            "actual WM_DESTROY of ready model persists current path");
        app::AppPrefs prefs;
        Check(prefs.Load() && prefs.language == L"zh-CN", "ready close persists current preferences");
        UnregisterClassW(ready_class.lpszClassName, instance);
    }
    UnregisterClassW(cls.lpszClassName, instance);
    OleUninitialize();
    std::printf("fixtures retained: %ls\n", root.c_str());
    return failures ? 1 : 0;
}
