#include "content_navigation.h"
#include <algorithm>

namespace pulse::app {
void RememberContentNavigation(Tab& tab) {
    if (!tab.content_results) return;
    auto saved = std::make_shared<ContentNavigationState>();
    saved->path = tab.current_path;
    saved->results = tab.content_results;
    saved->title = tab.virtual_title;
    saved->filter = tab.filter_text;
    saved->selected = tab.selected;
    saved->focus = tab.selected_index;
    saved->anchor = tab.selection_anchor;
    saved->all = tab.all_selected;
    saved->complete = tab.content_count_final;
    saved->x = tab.scroll_x; saved->y = tab.scroll_y;
    saved->view = tab.view_mode; saved->sort = tab.sort_column; saved->direction = tab.sort_direction;
    saved->sort_override = tab.content_sort_override;
    saved->input = tab.search_input_text; saved->root = tab.search_input_root;
    saved->current = tab.search_input_current; saved->content = tab.search_input_content;
    saved->relevance = tab.search_relevance;
    saved->banner = tab.banner_title; saved->message = tab.banner_message;
    std::erase_if(tab.content_navigation, [&](const auto& entry) { return entry->path == saved->path; });
    // Keep disk-backed results bounded without copying all result rows into RAM.
    if (tab.content_navigation.size() >= 4) tab.content_navigation.erase(tab.content_navigation.begin());
    tab.content_navigation.push_back(std::move(saved));
}

bool RestoreContentNavigation(Tab& tab) {
    const auto found = std::find_if(tab.content_navigation.begin(), tab.content_navigation.end(),
        [&](const auto& entry) { return entry->path == tab.current_path; });
    if (found == tab.content_navigation.end()) return false;
    const auto& saved = **found;
    tab.content_results = saved.results;
    tab.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
    tab.virtual_title = saved.title;
    tab.filter_text = saved.filter; tab.content_filter = saved.filter;
    tab.selected = saved.selected; tab.selected_index = saved.focus;
    tab.selection_anchor = saved.anchor; tab.all_selected = saved.all;
    ++tab.selection_revision;
    tab.content_count_final = saved.complete;
    tab.content_revision = saved.results->Revision();
    tab.scroll_x = saved.x; tab.scroll_y = saved.y;
    tab.view_mode = saved.view; tab.sort_column = saved.sort; tab.sort_direction = saved.direction;
    tab.content_sort_override = saved.sort_override;
    tab.search_input_path = saved.path; tab.search_input_text = saved.input;
    tab.search_input_root = saved.root; tab.search_input_current = saved.current;
    tab.search_input_content = saved.content; tab.search_relevance = saved.relevance;
    tab.banner_title = saved.banner; tab.banner_message = saved.message;
    tab.search_total = saved.results->Count(); tab.file_count = tab.search_total; tab.directory_count = 0;
    tab.loading = false;
    tab.search_content_stopped = true;
    tab.content_focus_path.clear();
    tab.search_preserve_selection.clear();
    tab.content_focus_selection = tab.content_focus_revision = UINT64_MAX;
    return true;
}
}
