// folder_views.h — What a folder was last left in: its view mode, sort order and details
// column edges. Explorer remembers these per folder; Pulse kept one answer for every folder
// until this store arrived.
#pragma once

#include "../ui/ui_renderer.h"
#include "../ui/view_layout.h"
#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace pulse {

struct AppState;

namespace app {

struct Tab;

struct FolderView {
    ui::ViewMode view = ui::ViewMode::Details;
    ui::SortColumn sort = ui::SortColumn::Name;
    ui::SortDirection direction = ui::SortDirection::Asc;
    std::array<float, 3> dividers{};
    uint64_t used = 0; // unix seconds; the save keeps the most recent kMaxEntries folders
};

// One JSON file maps folder -> FolderView. The window loads it once, notes the folder a tab
// is showing whenever the user changes that view, and applies the entry again when a tab
// navigates back to the folder. Saves merge what another window wrote in the meantime, so
// two windows do not undo each other (the rule places.json follows).
class FolderViewStore {
public:
    // Windows treats C:\Temp and c:\temp as one folder, so the map does.
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const noexcept;
    };
    using Map = std::map<std::wstring, FolderView, PathLess>;

    static constexpr size_t kMaxEntries = 500;

    bool persist = true; // self-test and shot runs can disable disk writes

    void Load();
    void Note(const std::wstring& path, const FolderView& view);
    const FolderView* Find(const std::wstring& path) const;
    void Save();
    size_t size() const noexcept { return views_.size(); }

private:
    Map views_;
    // What the file held at Load(). An entry that still equals it was not touched here, so a
    // save takes the disk copy instead - that is what keeps another window's writes alive.
    Map loaded_;
};

// Records the view a tab shows for its folder. Virtual views (search, recycle, tags) are not
// folders and are skipped.
void RememberFolderView(AppState& s, const Tab& tab);

// Applies the folder's memory to a tab that just navigated there. The tab keeps what it has
// when the folder has no memory; virtual views are never touched.
void ApplyFolderView(AppState& s, Tab& tab);

} // namespace app
} // namespace pulse
