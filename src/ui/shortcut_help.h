#pragma once
#include "ui_compositor.h"

namespace pulse::ui {
struct ShortcutHelpLayout {
    D2D1_RECT_F card{}, close{}, body{};
    float content_height = 0;
    float max_scroll = 0;
    bool narrow = false;
};
ShortcutHelpLayout LayoutShortcutHelp(float width, float height, float scale);
void DrawShortcutHelp(Compositor& compositor, bool dark, D2D1_COLOR_F accent,
                      float scale, float scroll, bool close_hover = false);
void ShowShortcutHelp(HWND owner, bool dark, D2D1_COLOR_F accent);
}
