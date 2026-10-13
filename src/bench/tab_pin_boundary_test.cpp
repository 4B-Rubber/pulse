#include "../app/vertical_tabs.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <set>

namespace pulse::app {
struct TabControllerTestAccess {
    static void TogglePin(TabController& controller, WindowTabs& tabs, size_t index) {
        controller.TogglePin(tabs, static_cast<int>(index));
    }
};
}

namespace {
using pulse::app::LayoutTab;
using pulse::app::WindowTabs;

bool Prefix(const WindowTabs& tabs) {
    bool unpinned = false;
    for (const auto& tab : tabs.items) {
        if (!tab->pinned) unpinned = true;
        else if (unpinned) return false;
    }
    return true;
}
bool Groups(const WindowTabs& tabs) {
    std::set<int> ended;
    int previous = 0;
    for (const auto& tab : tabs.items) {
        if (tab->pinned && tab->tab_group) return false;
        if (tab->tab_group != previous) {
            if (previous) ended.insert(previous);
            if (tab->tab_group && ended.contains(tab->tab_group)) return false;
        }
        previous = tab->tab_group;
    }
    return true;
}
std::array<LayoutTab*, 5> Seed(pulse::AppState& state) {
    state.window_tabs = WindowTabs{};
    std::array<LayoutTab*, 5> ids{};
    for (size_t i = 0; i < ids.size(); ++i) {
        ids[i] = &state.window_tabs.NewTab(L""); // This PC identity; no loading callback.
        ids[i]->pinned = i < 2;
    }
    return ids;
}
bool Order(const WindowTabs& tabs, std::initializer_list<LayoutTab*> expected) {
    return tabs.items.size() == expected.size() &&
        std::equal(tabs.items.begin(), tabs.items.end(), expected.begin(),
                   [](const auto& tab, const auto* id) { return tab.get() == id; });
}
}

bool RunTabPinBoundaryTest() {
    using namespace pulse;
    namespace fsys = std::filesystem;
    std::error_code error;
    fsys::create_directories(L"bench_data", error);
    FILE* log = nullptr;
    if (error || _wfopen_s(&log, L"bench_data/m10005_tab_pin_boundary.log", L"w") || !log) return false;
    int failures = 0, checks = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        fflush(log);
    };
    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->appPrefs.persist = state->searchHistory.persist = state->ctxMenuPrefs.persist = false;
    app::TabController controller; // No callbacks, providers or preferences writes.
    auto& tabs = state->window_tabs;
    for (size_t active = 0; active < 5; ++active) {
        auto ids = Seed(*state);
        tabs.SwitchTab(active);
        CommitVerticalTabDrag(*state, L"pulse:tab:4", 0);
        check(Order(tabs, {ids[0], ids[1], ids[4], ids[2], ids[3]}) && Prefix(tabs),
              "vertical ordinary drag before fixed tabs stops at normal boundary");
        check(tabs.Active() == ids[active], "ordinary cross-boundary drag preserves active identity");

        ids = Seed(*state);
        tabs.SwitchTab(active);
        CommitVerticalTabDrag(*state, L"pulse:tab:0", tabs.items.size());
        check(Order(tabs, {ids[1], ids[0], ids[2], ids[3], ids[4]}) && Prefix(tabs),
              "vertical fixed drag after ordinary tabs stops at fixed boundary");
        check(tabs.Active() == ids[active], "fixed cross-boundary drag preserves active identity");
    }
    auto ids = Seed(*state);
    CommitVerticalTabDrag(*state, L"pulse:tab:0", 2);
    CommitVerticalTabDrag(*state, L"pulse:tab:4", 2);
    check(Order(tabs, {ids[1], ids[0], ids[4], ids[2], ids[3]}) && Prefix(tabs),
          "vertical intra-region moves remain available in both regions");
    ids = Seed(*state);
    ids[2]->tab_group = ids[3]->tab_group = 7;
    tabs.tab_groups.emplace_back();
    tabs.tab_groups.back().id = 7;
    tabs.SwitchTab(3);
    CommitVerticalTabDrag(*state, L"pulse:tab:3", 0);
    check(Prefix(tabs) && Groups(tabs) && tabs.Active() == ids[3] && tabs.tab_groups.size() == 1 &&
          Order(tabs, {ids[0], ids[1], ids[3], ids[2], ids[4]}),
          "group member drag respects fixed prefix and normalizes contiguous group without changing active tab");

    for (size_t from = 0; from < 5; ++from) {
        for (size_t to = 0; to < 5; ++to) {
            ids = Seed(*state);
            tabs.SwitchTab(2);
            tabs.MoveTab(from, to);
            check(Prefix(tabs) && tabs.Active() == ids[2], "shared model move preserves pin prefix and active identity for every source/target");
        }
    }
    for (size_t index = 0; index < 5; ++index) {
        ids = Seed(*state);
        tabs.SwitchTab(index);
        const bool was_pinned = ids[index]->pinned;
        app::TabControllerTestAccess::TogglePin(controller, tabs, index);
        check(Prefix(tabs) && tabs.Active() == ids[index] && ids[index]->pinned != was_pinned,
              "real TogglePin transition preserves fixed prefix and active identity");
        const auto found = std::find_if(tabs.items.begin(), tabs.items.end(),
            [&](const auto& tab) { return tab.get() == ids[index]; });
        app::TabControllerTestAccess::TogglePin(controller, tabs, static_cast<size_t>(found - tabs.items.begin()));
        check(Prefix(tabs) && tabs.Active() == ids[index] && ids[index]->pinned == was_pinned,
              "real pin/unpin round trip preserves identity and restores pin state");
    }
    ids = Seed(*state);
    ids[4]->tab_group = 8;
    tabs.tab_groups.emplace_back();
    tabs.tab_groups.back().id = 8;
    app::TabControllerTestAccess::TogglePin(controller, tabs, 4);
    check(Prefix(tabs) && ids[4]->tab_group == 0 && tabs.tab_groups.empty(),
          "pinning grouped tab removes membership and prunes empty group");
    CommitVerticalTabDrag(*state, L"pulse:tab:4", 0);
    auto& created = tabs.NewTabAt(0, L"");
    check(Prefix(tabs) && !created.pinned && tabs.Active() == &created,
          "new tab insertion after vertical drag remains behind fixed prefix");

    // Orientation presentation consumes the same model; no geometry or mouse simulation.
    for (const bool vertical : {true, false, true}) {
        ui::WindowViewModel vm;
        for (size_t i = 0; i < tabs.items.size(); ++i) {
            ui::TabView tab;
            tab.title = std::to_wstring(i);
            tab.active = tabs.items[i].get() == tabs.Active();
            tab.pinned = tabs.items[i]->pinned;
            vm.tabs.push_back(std::move(tab));
        }
        const auto* active = tabs.Active();
        state->appPrefs.vertical_tabs = vertical;
        ApplyVerticalTabs(*state, vm);
        const auto group = std::find_if(vm.sidebar.begin(), vm.sidebar.end(),
            [](const auto& value) { return value.id == kVerticalTabsSectionId; });
        bool rows_match = !vertical ? group == vm.sidebar.end() : group != vm.sidebar.end();
        if (vertical && group != vm.sidebar.end()) {
            size_t rows = 0;
            for (const auto& row : group->items) if (row.tab_row) {
                rows_match = rows_match && row.path == L"pulse:tab:" + std::to_wstring(rows);
                ++rows;
            }
            rows_match = rows_match && rows == tabs.items.size();
        }
        check(rows_match && Prefix(tabs) && tabs.Active() == active,
              "vertical/horizontal presentation switch retains pin prefix and stable tab identities");
    }
    fprintf(log, "[SUMMARY] checks=%d failures=%d\n", checks, failures);
    fclose(log);
    return failures == 0;
}
