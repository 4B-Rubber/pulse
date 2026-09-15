#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include "../ui/window_material.h"

namespace pulse {
class GlobalSearchWindow {
public:
    GlobalSearchWindow();
    ~GlobalSearchWindow();
    GlobalSearchWindow(const GlobalSearchWindow&) = delete;
    GlobalSearchWindow& operator=(const GlobalSearchWindow&) = delete;
    bool Show(HWND owner, bool dark, float scale, const std::wstring& current_folder, bool search_pinyin = true);
    void SetAppearance(bool dark, ui::WindowEffect effect, const std::wstring& background_image, D2D1_COLOR_F accent);
    void Hide();
    void Shutdown();
    bool Visible() const;
private:
    friend struct GlobalSearchWindowTestPeer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
