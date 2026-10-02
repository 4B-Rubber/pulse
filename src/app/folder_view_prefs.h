#pragma once

#include "../ui/view_layout.h"
#include <map>
#include <optional>
#include <string>

namespace pulse::app {

class FolderViewPrefs {
public:
    std::optional<ui::ViewMode> Find(const std::wstring& path) const;
    bool Set(const std::wstring& path, ui::ViewMode mode);
    void Clear() { views_.clear(); }
    void AppendJson(std::wstring& out) const;
    void ReadJson(const std::wstring& json);

private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::map<std::wstring, ui::ViewMode, PathLess> views_;
};

} // namespace pulse::app
