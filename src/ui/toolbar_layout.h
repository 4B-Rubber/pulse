#pragma once
#include <d2d1.h>
#include <algorithm>
#include <array>

namespace pulse::ui {
struct ToolbarLayout {
    std::array<D2D1_RECT_F, 4> navigation{};
    D2D1_RECT_F address{}, search{}, create{};
    D2D1_RECT_F sort{}, filter{}, overflow{};
    std::array<D2D1_RECT_F, 8> commands{};
};

inline ToolbarLayout MakeToolbarLayout(float width, float scale, float top, float margin, float create_width,
                                      float left, float filter_expand = 0.0f) {
    ToolbarLayout out;
    const float available = width - left;
    const float nav_step = available < 500*scale ? 26*scale : 34*scale;
    float x = left + margin;
    for (int i=0;i<3;++i) {
        out.navigation[i] = {x,top+6*scale,x+nav_step-4*scale,top+38*scale};
        x += nav_step;
    }
    const float search_width = std::clamp(available*0.27f, 140*scale, 360*scale);
    out.search = {width-margin-search_width,top+4*scale,width-margin,top+40*scale};
    out.address = {x+margin,top+4*scale,std::max(x+margin+60*scale,out.search.left-margin),top+40*scale};
    const bool overflow = available < 540*scale;
    if (available < 480*scale) {
        out.search.left=out.search.right-32*scale;
    }
    out.navigation[3] = {out.search.left-margin-nav_step,top+6*scale,
                         out.search.left-margin-4*scale,top+38*scale};
    out.address.right = out.navigation[3].left-margin;
    out.create = {left+margin,top+50*scale,left+margin+create_width,top+82*scale};
    x = out.create.right + 12*scale;
    for (int i=0;i<5;++i) {
        out.commands[i] = {x,top+50*scale,x+30*scale,top+82*scale};
        x += 34*scale;
    }
    if (overflow) { for (auto& command : out.commands) command = {}; x = out.create.right; }
    x += 8*scale;
    const float sort_width = available >= 700*scale ? 88*scale : 32*scale;
    out.sort = {x,top+50*scale,x+sort_width,top+82*scale};
    x = out.sort.right + 6*scale;
    const float filter_base = available >= 700*scale ? 88*scale : 32*scale;
    const float filter_max = std::clamp(width-margin-(overflow ? 44 : 114)*scale-x,filter_base,220*scale);
    const float filter_width = filter_base + (filter_max-filter_base)*std::clamp(filter_expand,0.0f,1.0f);
    out.filter = {x,top+50*scale,x+filter_width,top+82*scale};
    const float right_group = std::max(out.filter.right+12*scale,width-margin-102*scale);
    for (int i=5;i<8;++i) {
        x = right_group+(i-5)*34*scale;
        out.commands[i] = {x,top+50*scale,x+30*scale,top+82*scale};
    }
    if (overflow) {
        out.commands[5] = out.commands[6] = out.commands[7] = {};
        out.overflow = {width-margin-32*scale,top+50*scale,width-margin,top+82*scale};
    }
    return out;
}
}
