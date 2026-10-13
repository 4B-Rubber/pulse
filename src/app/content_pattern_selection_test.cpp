#ifdef PULSE_WITH_SELFTEST
#include "app_runtime.h"
#include "app_navigation.h"
#include "content_results_ui.h"
#include <cstdio>
#include <filesystem>
#include <atomic>

bool RunContentPatternSelectionTest() {
    using namespace pulse;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/m11005_content_pattern_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log);
        failures += !ok;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true; s.shot.active = true;
    s.appPrefs.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 960, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.window_tabs.NewTab(L"pulse:search?content:fixture");
    BindCurrentLayout(s);
    auto wait = [&](auto ready) {
        const auto until = GetTickCount64() + 5000;
        while (!ready() && GetTickCount64() < until) {
            RefreshContentResults(s); CompleteContentSelection(s); Sleep(1);
        }
        RefreshContentResults(s); CompleteContentSelection(s);
        return ready();
    };
    struct Barrier {
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE exited = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Barrier() { CloseHandle(entered); CloseHandle(release); CloseHandle(exited); }
    };
    for (int scenario = 0; scenario < 10; ++scenario) {
        fprintf(log, "scenario=%d\n", scenario);
        s.contentSelectionAction.reset();
        auto* tab = ActiveTab(s);
        *tab = app::Tab{};
        tab->current_path = L"pulse:search?content:fixture";
        auto store = std::make_shared<index::ContentResultStore>(nullptr, 0);
        std::vector<index::ContentHit> hits;
        for (const auto* name : {L"a.keep", L"b.other", L"c.keep"}) {
            index::ContentHit hit; hit.name = name;
            hit.path = std::wstring(L"C:\\content-pattern-fixture\\") + name;
            hits.push_back(std::move(hit));
        }
        check(store->Append(hits), "private SQLite fixture populated");
        tab->content_results = store;
        tab->SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
        tab->filter_text = scenario == 1 ? L"" : L"*.keep";
        RefreshContentResults(s);
        check(wait([&] { return !store->Filtering(); }), "initial filter completed");
        auto gate = std::make_shared<Barrier>();
        store->Resolve({}, false, 0, [gate](auto) {
            SetEvent(gate->entered); WaitForSingleObject(gate->release, 10000); SetEvent(gate->exited);
        });
        const bool blocked = WaitForSingleObject(gate->entered, 5000) == WAIT_OBJECT_0;
        check(blocked, "actual store worker held before queued filter clear");
        if (scenario == 9) {
            tab->search_preserve_selection = L"C:\\content-pattern-fixture\\b.other";
            tab->content_selected_paths = {tab->search_preserve_selection};
        }
        SelectContentPattern(s, scenario == 2 ? L"*.missing" : L"*.keep");
        check(s.contentSelectionAction != nullptr, "pattern remains pending while worker is held");
        if (scenario == 3) tab->ClearSelection();
        if (scenario == 4) tab->content_results = std::make_shared<index::ContentResultStore>(nullptr, 0);
        if (scenario == 5) CancelContentSelection(s, *tab);
        if (scenario == 6) SelectContentPattern(s, L"*.other");
        if (scenario == 7) { tab->filter_text = L"*.other"; RefreshContentResults(s); }
        if (scenario == 8) store->SetSort(index::ContentResultSort::Name, true);
        SetEvent(gate->release);
        check(WaitForSingleObject(gate->exited, 5000) == WAIT_OBJECT_0, "worker barrier released");
        check(wait([&] { return !store->Filtering() && !store->Sorting() && !s.contentSelectionAction; }),
            "pattern transition completes without stale action");
        const bool cancelled = (scenario >= 3 && scenario <= 5) || scenario == 7;
        if (cancelled || scenario == 2) {
            check(tab->SelectedCount() == 0, "cancelled or zero-match pattern leaves no selection");
        } else {
            std::vector<std::wstring> paths;
            bool invoked = false;
            check(DeferContentSelection(s, [&](AppState& current) {
                paths = SelectedFullPaths(*ActiveTab(current)); invoked = true;
            }), "selected operation resolves through production async path");
            check(wait([&] { return !s.contentSelectionAction; }) && invoked,
                "operation accepted at final display order");
            std::sort(paths.begin(), paths.end());
            const std::vector<std::wstring> expected = scenario == 6 ?
                std::vector<std::wstring>{L"C:\\content-pattern-fixture\\b.other"} :
                std::vector<std::wstring>{L"C:\\content-pattern-fixture\\a.keep", L"C:\\content-pattern-fixture\\c.keep"};
            check(paths == expected, "exact matched file identities reach operation");
        }
        tab->content_results.reset();
    }
    s.contentSelectionAction.reset();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    fprintf(log, "failures=%d\n", failures); fclose(log);
    return failures == 0;
}
#endif
