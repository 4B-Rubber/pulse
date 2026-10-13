#pragma once
#include "app_model.h"

namespace pulse::app {
struct ContentNavigationState {
    std::wstring path, title, filter, input, root, banner, message;
    std::shared_ptr<index::ContentResultStore> results;
    uint64_t order_revision = 0;
    std::unordered_set<int> selected;
    uint64_t order = 0;
    std::vector<std::wstring> selected_paths;
    std::wstring focus_path;
    int focus = -1, anchor = -1;
    bool all = false, complete = false, current = false, content = true, relevance = true;
    float x = 0, y = 0;
    ui::ViewMode view = ui::ViewMode::Details;
    ui::SortColumn sort = ui::SortColumn::Name;
    ui::SortDirection direction = ui::SortDirection::Asc;
    bool sort_override = false;
};
void RememberContentNavigation(Tab& tab);
bool RestoreContentNavigation(Tab& tab);
}
