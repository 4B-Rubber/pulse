// folder_views.cpp — Storage for the per-folder view memory.
#include "folder_views.h"

#include "../common/json_utils.h"
#include "../common/scaled_edges.h"
#include "../common/utf8_file.h"
#include "../fs/fs_enum.h"
#include "app_model.h"
#include "app_state.h"
#include "session.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <windows.h>

namespace pulse::app {

namespace {

constexpr wchar_t kStoreFile[] = L"folder-views.json";

std::wstring StorePath() {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return {};
    return dir + L"\\" + kStoreFile;
}

uint64_t NowUsed() {
    return static_cast<uint64_t>(time(nullptr));
}

const wchar_t* ColumnName(ui::SortColumn column) {
    switch (column) {
    case ui::SortColumn::Mtime: return L"mtime";
    case ui::SortColumn::Type: return L"type";
    case ui::SortColumn::Size: return L"size";
    case ui::SortColumn::Path: return L"path";
    case ui::SortColumn::Name: default: return L"name";
    }
}

ui::SortColumn ParseColumn(const std::wstring& value) {
    if (value == L"mtime") return ui::SortColumn::Mtime;
    if (value == L"type") return ui::SortColumn::Type;
    if (value == L"size") return ui::SortColumn::Size;
    if (value == L"path") return ui::SortColumn::Path;
    return ui::SortColumn::Name;
}

// "used" is written as a bare number, so it needs its own reader (ExtractString wants a
// quoted value and would hand back zero for every entry).
uint64_t ExtractUsed(const std::wstring& block) {
    const size_t pos = pulse::json::ValuePosition(block, L"used");
    if (pos == std::wstring::npos || pos >= block.size()) return 0;
    return _wcstoui64(block.c_str() + pos, nullptr, 10);
}

// "This PC" lists the drives with an empty path: it is a location like any folder, so it gets a
// key of its own instead of being skipped. Real virtual views (search, recent, a tag, recycle)
// are not folders and keep being skipped. The store is ours, so a pulse: key is free to use.
constexpr wchar_t kThisPcKey[] = L"pulse:this-pc";

std::wstring StoreKey(const std::wstring& path) {
    const std::wstring folder = fs::NormalizePath(path);
    if (folder.empty()) return kThisPcKey;
    if (fs::IsVirtualPath(folder)) return std::wstring();
    return folder;
}

FolderView ParseEntry(const std::wstring& block) {
    FolderView view;
    view.view = ui::ParseViewMode(pulse::json::ExtractString(block, L"view"));
    view.sort = ParseColumn(pulse::json::ExtractString(block, L"sort"));
    view.direction = pulse::json::ExtractString(block, L"direction") == L"desc"
        ? ui::SortDirection::Desc : ui::SortDirection::Asc;
    view.columns = pulse::json::ExtractBool(block, L"columns");
    view.dividers = ParseScaled3(pulse::json::ExtractString(block, L"cols"));
    view.used = ExtractUsed(block);
    return view;
}

FolderViewStore::Map ParseStore(const std::wstring& json) {
    FolderViewStore::Map views;
    for (const auto& block : pulse::json::ExtractObjectArray(json, L"folder_views")) {
        const std::wstring folder = StoreKey(pulse::json::ExtractString(block, L"path"));
        if (folder.empty()) continue;
        views[folder] = ParseEntry(block);
    }
    return views;
}

// The fields that decide what the folder looks like; `used` is bookkeeping and stays out, so
// two entries differing only in it count as the same view.
bool SameView(const FolderView& a, const FolderView& b) {
    return a.view == b.view && a.sort == b.sort && a.direction == b.direction &&
           a.columns == b.columns && a.dividers == b.dividers;
}

void TrimOldest(FolderViewStore::Map& views) {
    if (views.size() <= FolderViewStore::kMaxEntries) return;
    std::vector<std::pair<uint64_t, std::wstring>> order;
    order.reserve(views.size());
    for (const auto& entry : views) order.emplace_back(entry.second.used, entry.first);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    for (size_t i = FolderViewStore::kMaxEntries; i < order.size(); ++i)
        views.erase(order[i].second);
}

std::wstring Serialize(const FolderViewStore::Map& views) {
    std::wostringstream out;
    out << L"{\"version\":1,\"folder_views\":[";
    bool first = true;
    for (const auto& entry : views) {
        if (!first) out << L",";
        first = false;
        std::wstring escaped;
        pulse::json::Escape(entry.first, escaped);
        out << L"{\"path\":\"" << escaped
            << L"\",\"view\":\"" << ui::ViewModeName(entry.second.view)
            << L"\",\"sort\":\"" << ColumnName(entry.second.sort)
            << L"\",\"direction\":\""
            << (entry.second.direction == ui::SortDirection::Desc ? L"desc" : L"asc")
            << L"\",\"columns\":" << (entry.second.columns ? L"true" : L"false")
            << L",\"cols\":\"" << FormatScaled3(entry.second.dividers)
            << L"\",\"used\":" << entry.second.used << L"}";
    }
    out << L"]}";
    return out.str();
}

} // namespace

bool FolderViewStore::PathLess::operator()(const std::wstring& a,
                                           const std::wstring& b) const noexcept {
    const int order = CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                           b.c_str(), static_cast<int>(b.size()), TRUE);
    return order == CSTR_LESS_THAN;
}

void FolderViewStore::Load() {
    views_.clear();
    loaded_.clear();
    if (!persist) return;
    const std::wstring path = StorePath();
    if (path.empty()) return;
    std::wstring json;
    if (!ReadUtf8File(path, json) || json.empty()) return;
    views_ = ParseStore(json);
    TrimOldest(views_);
    loaded_ = views_;
}

void FolderViewStore::Note(const std::wstring& path, const FolderView& view) {
    if (!persist) return;
    const std::wstring folder = StoreKey(path);
    if (folder.empty()) return;
    FolderView entry = view;
    // A caller that leaves `used` at zero means "now"; the self test sets it to stage a
    // sequence of visits that the trimming can then order.
    if (entry.used == 0) entry.used = NowUsed();
    views_[folder] = entry;
    Save();
}

void FolderViewStore::Touch(const std::wstring& path) {
    if (!persist) return;
    const std::wstring folder = StoreKey(path);
    if (folder.empty()) return;
    const auto it = views_.find(folder);
    if (it == views_.end()) return;
    const uint64_t now = NowUsed();
    if (it->second.used == now) return;
    it->second.used = now;
    visited_ = true;
}

void FolderViewStore::SaveIfVisited() {
    if (!visited_) return;
    visited_ = false;
    Save();
}

const FolderView* FolderViewStore::Find(const std::wstring& path) const {
    // Keys went through StoreKey, so the lookup has to as well ("This PC" included).
    const std::wstring folder = StoreKey(path);
    if (folder.empty()) return nullptr;
    const auto it = views_.find(folder);
    return it == views_.end() ? nullptr : &it->second;
}

void FolderViewStore::Save() {
    if (!persist) return;
    const std::wstring path = StorePath();
    if (path.empty()) return;
    // Read the file so entries another window wrote while this one was up survive: an entry
    // that still equals what Load() read was not touched here, so the disk copy wins; an
    // entry this window never saw is another window's and comes along.
    Map merged;
    std::wstring json;
    Map disk;
    if (ReadUtf8File(path, json) && !json.empty()) disk = ParseStore(json);
    for (const auto& entry : views_) {
        const auto base = loaded_.find(entry.first);
        if (base != loaded_.end() && SameView(base->second, entry.second)) {
            const auto other = disk.find(entry.first);
            if (other != disk.end()) {
                // The view is the disk copy's - this window never changed it - but the visit this
                // window just recorded is newer than what the other window wrote, and trimming has
                // to keep the most recently used folders.
                FolderView kept = other->second;
                kept.used = (std::max)(kept.used, entry.second.used);
                merged[entry.first] = kept;
            }
            continue;
        }
        merged[entry.first] = entry.second;
    }
    for (const auto& entry : disk) {
        if (views_.count(entry.first) || loaded_.count(entry.first)) continue;
        merged[entry.first] = entry.second;
    }
    TrimOldest(merged);
    KeepPreviousFileCopy(path);
    if (!WriteUtf8FileAtomic(path, Serialize(merged))) return;
    views_ = merged;
    loaded_ = merged;
}

void RememberFolderView(AppState& s, const Tab& tab) {
    FolderView view;
    view.view = tab.view_mode;
    view.sort = tab.sort_column;
    view.direction = tab.sort_direction;
    view.columns = tab.column_layout;
    view.dividers = tab.details_column_dividers;
    // The store maps the key itself: "This PC" has an empty path and is a location, while
    // search results, recent, tags and recycle are not folders and are dropped there.
    s.folderViews.Note(tab.current_path, view);
}

void RememberFolderViewMode(AppState& s, const std::wstring& path, ui::ViewMode view) {
    FolderView entry;
    if (const FolderView* existing = s.folderViews.Find(path)) {
        if (existing->view == view) return; // nothing new to record
        entry = *existing;
    }
    entry.view = view;
    s.folderViews.Note(path, entry);
}

void ApplyFolderView(AppState& s, Tab& tab) {
    if (const FolderView* memory = s.folderViews.Find(tab.current_path)) {
        tab.view_mode = memory->view;
        tab.sort_column = memory->sort;
        tab.sort_direction = memory->direction;
        tab.details_column_dividers = memory->dividers;
        // The column layout brings the details panel with it, the way toggling it does.
        if (tab.column_layout != memory->columns) {
            tab.column_layout = memory->columns;
            if (!tab.column_layout) {
                tab.column_strip = {};
            } else if (!s.showDetailsPanel) {
                s.showDetailsPanel = true;
                s.renderer.SetDetailsPanelVisible(true);
            }
        }
        // The folder has just been entered: keep it at the front of the save order.
        s.folderViews.Touch(tab.current_path);
    }
}

} // namespace pulse::app
