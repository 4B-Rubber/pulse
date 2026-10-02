#ifdef PULSE_CONTENT_NAVIGATION_STANDALONE
#include "app_internal.h"
#include "content_navigation.h"
#include "../common/localization.h"
#include <cstdio>

int wmain() {
    using namespace pulse;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->appPrefs.persist = false;
    state->settings.BindUi(state->appPrefs, state->ctxMenuPrefs, state->index, state->networkIndex, {});
    state->hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 900, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const std::wstring query = app::MakeSearchPath(L"content:needle");
    state->window_tabs.NewTab(query);
    state->pane = state->window_tabs.Active()->panes.front().get();
    auto* tab = ActiveTab(*state);
    auto results = std::make_shared<index::ContentResultStore>(nullptr, 0);
    index::ContentHit hit;
    hit.path = L"C:\\PulseNavigationFixture\\hit.txt"; hit.name = L"hit.txt";
    bool ok = results->Append({hit});
    tab->content_results = results;
    tab->content_count_final = true;
    tab->search_total = 1;
    tab->search_input_path = query;
    tab->search_input_text = L"needle";
    tab->search_input_content = true;
    tab->SelectOnly(0);
    tab->scroll_y = 120;
    state->addressLiveDue = 123;
    auto check = [&](bool passed, const char* label) {
        printf("[%s] %s\n", passed ? "PASS" : "FAIL", label); ok &= passed;
    };
    NavigateTo(*state, L"C:\\PulseNavigationFixture");
    check(!tab->content_results && state->addressLiveDue == 0, "leaving search clears pending query but retains history");
    GoBack(*state);
    check(tab->current_path == query && tab->content_results == results,
        "back restores the same paged results without a new search");
    check(tab->selected_index == 0 && tab->scroll_y == 120 && tab->search_input_text == L"needle",
        "back restores selection, scroll and search draft");
    check(!tab->loading && !tab->pending_generation && !tab->search_content_active && tab->content_count_final,
        "restored completed results do not restart scanning");
    GoForward(*state);
    GoBack(*state);
    check(tab->content_results == results && tab->selected_index == 0, "repeated forward/back preserves results");
    tab->search_content_active = true;
    tab->content_count_final = false;
    NavigateTo(*state, L"C:\\PulseNavigationFixture");
    GoBack(*state);
    check(tab->content_results == results && !tab->content_count_final && tab->search_content_stopped &&
        !tab->search_content_active, "unfinished search returns as retained partial results without a restart");
    for (int i = 0; i < 6; ++i) {
        tab->current_path = app::MakeSearchPath(L"content:" + std::to_wstring(i));
        app::RememberContentNavigation(*tab);
    }
    check(tab->content_navigation.size() == 4, "retained result stores have a bounded lifetime");
    DestroyWindow(state->hwnd);
    state->hwnd = nullptr;
    state.reset();
    CoUninitialize();
    return ok ? 0 : 1;
}
#endif
