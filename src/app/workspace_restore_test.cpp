#ifdef PULSE_WITH_SELFTEST
#include "app_navigation.h"
#include "app_runtime.h"
#include <filesystem>
#include <cstdio>

bool RunWorkspaceRestoreTest() {
    using namespace pulse;
    namespace fsys = std::filesystem;
    int failures = 0;
    const auto parent = fsys::absolute(L"bench_data").lexically_normal();
    const auto base = parent / (L"workspace-restore-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fsys::create_directories(parent);
    if (!fsys::create_directory(base)) return false;
    FILE* log = nullptr;
    _wfopen_s(&log, (parent / L"m10004_workspace_test.log").c_str(), L"w");
    auto check = [&](bool ok, const char* text) {
        if (log) { fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", text); fflush(log); }
        failures += !ok;
    };
    check(log != nullptr, "workspace regression log opened");
    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous, ARRAYSIZE(previous));
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", base.c_str());
    std::vector<std::wstring> folders;
    for (const auto name : {L"one", L"two", L"three", L"other"}) {
        fsys::create_directory(base / name); folders.push_back((base / name).wstring());
    }
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto state = std::make_unique<AppState>();
        state->isolatedTest = true;
        state->shot.active = true;
        state->appPrefs.persist = state->searchHistory.persist = state->ctxMenuPrefs.persist = false;
        state->hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 960, 600,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        app::Workspace workspace;
        workspace.name = L"Private workspace"; workspace.root = folders[0];
        workspace.layout = static_cast<int>(app::LayoutPreset::Three);
        if (scenario != 4) {
            workspace.pane_paths = {folders[0], folders[1], folders[2]};
            if (scenario < 3) workspace.pane_paths[scenario].clear();
            workspace.pane_views = {ui::ViewMode::Details, ui::ViewMode::List, ui::ViewMode::LargeIcons};
        }
        state->places.workspaces = {workspace};
        check(state->places.Save(), "save workspace to private profile");
        state->places.workspaces.clear();
        check(state->places.Load() && state->places.workspaces.size() == 1,
            "reload saved pane paths and view modes");
        // The original tab is closed; another unrelated tab remains open.
        state->window_tabs.NewTab(folders[3]);
        state->window_tabs.NewTab(folders[0]);
        state->window_tabs.CloseTab(1);
        BindCurrentLayout(*state);
        OpenWorkspace(*state, 0);
        std::vector<app::Pane*> visible;
        if (Root(*state)) Root(*state)->CollectPanes(visible);
        bool correct = visible.size() == 3;
        for (size_t i = 0; correct && i < visible.size(); ++i) {
            const auto* tab = visible[i]->ActiveTab();
            const auto expected = scenario == 4 ? workspace.root : workspace.pane_paths[i];
            correct = tab && tab->current_path == fs::NormalizePath(expected) &&
                (scenario == 4 || tab->view_mode == workspace.pane_views[i]);
        }
        check(correct, scenario == 4 ? "legacy absent pane_paths keeps root fallback" :
            "actual visible panes restore This PC identity, saved order and views");
        check(state->window_tabs.items.size() == 2, "reopening workspace preserves the unrelated tab");
        state->watches.Stop(); state->worker.Stop();
        DestroyWindow(state->hwnd); state->hwnd = nullptr;
    }
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous[0] ? previous : nullptr);
    std::error_code error;
    if (base.parent_path() == parent && base.filename().wstring().starts_with(L"workspace-restore-"))
        fsys::remove_all(base, error);
    check(!error && !fsys::exists(base), "owned workspace profile and folders removed");
    if (log) fclose(log);
    return failures == 0;
}
#endif
