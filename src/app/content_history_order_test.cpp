#ifdef PULSE_WITH_SELFTEST
#include "app_internal.h"
#include "content_navigation.h"
#include <cstdio>
#include <filesystem>
#include <future>

bool RunContentHistoryOrderTest() {
    using namespace pulse;
    using Store = index::ContentResultStore;
    int failures = 0;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/m11006_content_history_test.log", L"w");
    auto check = [&](bool ok, const char* label) {
        if (log) { fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log); }
        failures += !ok;
        return ok;
    };
    auto drain = [](const std::shared_ptr<Store>& store) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        store->Resolve({}, false, 0, [done](auto) { done->set_value(); });
        return future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    };
    struct Barrier {
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Barrier() { CloseHandle(entered); CloseHandle(release); }
    };
    check(log != nullptr, "regression log opened");
    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->places.persist = false;
    state->appPrefs.persist = state->searchHistory.persist = state->ctxMenuPrefs.persist = false;
    state->hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 900, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto query = app::MakeSearchPath(L"content:history-order-fixture");
    state->window_tabs.NewTab(query);
    state->pane = state->window_tabs.Active()->panes.front().get();
    auto* tab = ActiveTab(*state);
    auto make_store = [&] {
        auto store = std::make_shared<Store>(nullptr, 0);
        index::ContentHit a, b;
        a.path = L"C:\\history-order-fixture\\a.txt"; a.name = L"a.txt";
        b.path = L"C:\\history-order-fixture\\b.txt"; b.name = L"b.txt";
        check(store->Append({a, b}), "append isolated disk-backed results");
        return store;
    };
    auto a = make_store();
    auto b = make_store();
    a->SetSort(index::ContentResultSort::Name, false);
    check(drain(a), "initial A sort completes");
    b->SetFilter([](const fs::DirEntry&) { return true; });
    check(drain(b), "B filter creates a distinct order");
    auto attach = [&](const std::shared_ptr<Store>& store) {
        tab->content_results = store;
        tab->content_order_revision = store->OrderRevision();
        tab->content_revision = store->Revision();
        tab->search_total = store->Count();
        tab->SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
        tab->SelectOnly(0);
    };
    auto operate = [&](const std::wstring& expected, bool focused) {
        bool invoked = false;
        std::wstring path;
        check(DeferContentSelection(*state, [&](AppState&) {
            invoked = true;
            path = tab->EntryAt(static_cast<size_t>(tab->selected_index)).full_path;
        }, focused), "real deferred selection queued");
        const bool completed = check(drain(tab->content_results), "deferred resolve callback completes");
        if (completed) CompleteContentSelection(*state);
        else state->contentSelectionAction.reset();
        check(invoked && path == expected, "deferred action resolves the correct restored-store path");
    };
    attach(a);
    tab->content_count_final = true;
    app::RememberContentNavigation(*tab);
    attach(b);
    check(a->OrderRevision() != b->OrderRevision(), "A and B carry different order revisions");
    check(app::RestoreContentNavigation(*tab) && tab->content_results == a &&
        tab->content_order_revision == a->OrderRevision() && tab->selected_index == 0,
        "unchanged A history restores its own order and selection");
    operate(L"C:\\history-order-fixture\\a.txt", true);
    tab->SelectOnly(1);
    operate(L"C:\\history-order-fixture\\b.txt", false);
    for (bool filter : {false, true}) {
        attach(a);
        tab->content_count_final = false;
        auto gate = std::make_shared<Barrier>();
        a->Resolve({}, false, 0, [gate](auto) {
            SetEvent(gate->entered);
            WaitForSingleObject(gate->release, 10000);
        });
        const bool entered = WaitForSingleObject(gate->entered, 10000) == WAIT_OBJECT_0;
        check(entered, "real store callback blocks its serial queue");
        if (filter) a->SetFilter([](const fs::DirEntry& entry) { return entry.name == L"b.txt"; });
        else a->SetSort(index::ContentResultSort::Name, true);
        app::RememberContentNavigation(*tab);
        attach(b);
        check(app::RestoreContentNavigation(*tab) && tab->SelectedCount() == 0,
            "return while queued reorder is busy safely clears saved indices");
        SetEvent(gate->release);
        check(drain(a), "late queued sort or filter completes after history capture");
        attach(b);
        tab->content_selected_paths = {L"C:\\unrelated-B.txt"};
        check(app::RestoreContentNavigation(*tab) && tab->SelectedCount() == 0 &&
            tab->content_selected_paths.empty() && tab->content_order_revision == a->OrderRevision(),
            "late order change never labels old indices as current or reuses B identity cache");
        check(!tab->content_count_final && tab->search_content_stopped && !tab->loading,
            "partial retained history remains stopped without restarting clients");
        tab->SelectOnly(0);
        operate(L"C:\\history-order-fixture\\b.txt", false);
    }
    // Simulate an old display token with current count/data revision: the UI
    // must repair this independently of any additional incoming result batch.
    tab->content_revision = a->Revision();
    tab->search_total = a->Count();
    tab->content_order_revision = UINT64_MAX;
    RefreshContentResults(*state);
    check(tab->content_order_revision == a->OrderRevision(), "refresh repairs order mismatch with unchanged count and data revision");
    DestroyWindow(state->hwnd);
    state->hwnd = nullptr;
    const auto a_cache = a->CachePath(), b_cache = b->CachePath();
    state.reset();
    a.reset(); b.reset();
    // ContentResultStore signals its detached worker; Impl closes SQLite and
    // deletes the spool only after that worker has released its final reference.
    auto removed = [](const std::wstring& path) {
        if (path.empty()) return false;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    };
    const ULONGLONG cleanup_deadline = GetTickCount64() + 10000;
    while ((!removed(a_cache) || !removed(b_cache)) && GetTickCount64() < cleanup_deadline)
        Sleep(5);
    const bool cleaned = removed(a_cache) && removed(b_cache);
    if (!cleaned && log) {
        for (const auto& path : {a_cache, b_cache}) {
            const DWORD attrs = GetFileAttributesW(path.c_str());
            const DWORD error = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
            fprintf(log, "cleanup path=%ls attrs=%lu error=%lu\n", path.c_str(), attrs, error);
        }
    }
    check(cleaned, "private result spools are removed");
    if (log) { fprintf(log, "failures=%d\n", failures); fclose(log); }
    return failures == 0;
}
#endif
