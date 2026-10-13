#ifdef PULSE_WITH_SELFTEST
#include "app_commands.h"
#include "app_navigation.h"
#include "../common/localization.h"
#include <cstdio>

bool RunTagDeleteViewTest() {
    using namespace pulse;
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/tag_delete_view_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", text);
        fflush(log); failures += !ok;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true; s.shot.active = true;
    s.appPrefs.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"Private tag deletion test", WS_POPUP,
        0, 0, 600, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(s.hwnd != nullptr, "create hidden isolated owner");
    if (!s.hwnd) { fclose(log); return false; }
    const auto removed = s.places.CreateTag(L"Removed fixture tag", 0x123456);
    const auto retained = s.places.CreateTag(L"Retained fixture tag", 0x654321);
    auto add = [&](const std::wstring& path) {
        s.window_tabs.NewTab(path);
        auto* tab = s.window_tabs.Active()->ActiveFolder();
        auto rows = std::make_shared<std::vector<fs::DirEntry>>(1);
        rows->front().name = L"old association.txt";
        tab->SetSnapshot(rows); tab->SelectOnly(0);
        return tab;
    };
    auto* first = add(app::MakeTagPath(removed));
    auto* second = add(app::MakeTagPath(removed));
    auto* legacy = add(L"pulse:tag:0");
    auto* other = add(app::MakeTagPath(retained));
    auto* directory = add(L"C:\\private-tag-view-no-io");
    BindCurrentLayout(s);
    const auto directory_snapshot = directory->snapshot;
    const auto other_snapshot = other->snapshot;
    for (auto* tab : {first, second, legacy}) {
        tab->virtual_title = L"Removed fixture tag";
        tab->loading = true; tab->pending_generation = 991;
    }
    check(DeleteTagAndRefreshViews(s, removed), "production delete command succeeds");
    check(!s.places.FindTag(removed) && s.places.FindTag(retained), "only requested catalog tag removed");
    for (auto* tab : {first, second, legacy}) {
        check(tab->snapshot && tab->EntryCount() == 0 && tab->SelectedCount() == 0,
            "every associated background page drops stale rows and selection immediately");
        check(!tab->loading && tab->pending_generation == 0, "deleted page cancels pending listing identity");
        check(tab->virtual_title == l10n::Pick(L"标签已删除", L"Tag deleted"),
            "deleted page has readable title instead of old name or UUID");
    }
    app::WorkResult stale{};
    stale.path = app::MakeTagPath(removed); stale.generation = 991; stale.snapshot = directory_snapshot;
    ApplyWorkerResult(s, stale);
    check(first->EntryCount() == 0 && second->EntryCount() == 0, "late old listing cannot repopulate deleted pages");
    check(directory->snapshot == directory_snapshot && directory->SelectedCount() == 1 &&
        ActiveTab(s) == directory, "ordinary active directory keeps rows selection and focus");
    check(other->snapshot == other_snapshot && other->SelectedCount() == 1,
        "unrelated tag page keeps rows and selection");
    LoadVirtualView(s, *first, first->current_path);
    check(first->EntryCount() == 0 && first->virtual_title == l10n::Pick(L"标签已删除", L"Tag deleted"),
        "later refresh retains empty readable deleted page");
    check(!DeleteTagAndRefreshViews(s, removed), "repeated deletion is harmless");
    s.window_tabs.active = 3;
    BindCurrentLayout(s);
    check(DeleteTagAndRefreshViews(s, retained) && ActiveTab(s) == other && other->EntryCount() == 0 &&
        other->virtual_title == l10n::Pick(L"标签已删除", L"Tag deleted"),
        "deleting active tag clears it without switching the current page");
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    fprintf(log, "Failures: %d\n", failures); fclose(log);
    return failures == 0;
}
#endif
