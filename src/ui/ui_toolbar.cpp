#include "ui_renderer_internal.h"
#include "toolbar_layout.h"
#include "pane_header_icons.h"

namespace pulse::ui {
void MainRenderer::DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    auto* dc = compositor_->Dc();
    const float left = EffectiveSidebarWidth(rect.right);
    const bool compact = rect.right-left < 600*scale_;
    const auto layout = ToolbarLayoutAt(rect.right,NewButtonWidthPx(compact),vm.pane.filter_expand);
    const bool searchOverlay=vm.address_searching && rect.right-left<480*scale_;
    if (!searchOverlay) {
        const wchar_t* nav_glyphs[] = {kIconBack,kIconForward,kIconUp,kIconRefresh};
        const HitTestResult::Region nav_hits[] = {HitTestResult::NavBack,HitTestResult::NavForward,HitTestResult::NavUp,HitTestResult::NavRefresh};
        for (int i=0;i<4;++i) {
            const auto r=layout.navigation[i];
            if (r.right<=r.left) continue;
            const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward : true;
            DrawButton(r,theme,enabled && IsHovered(vm,nav_hits[i]) ? theme.fill_hover : kTransparent,
                nav_glyphs[i],L"",enabled ? theme.text_secondary : theme.text_disabled,true,true);
        }
        // Breadcrumb address bar: segments clickable, empty area -> edit mode.
        D2D1_RECT_F addrRc = AddressBarRect(rect.right);
        fluent::ControlState addrState{};
        addrState.focused = vm.address_editing && !vm.address_searching;
        addrState.hovered = !vm.address_editing && IsHovered(vm, HitTestResult::AddressBar);
        painter_.DrawTextFieldFrame(addrRc, addrState);
        dc->PushAxisAlignedClip(addrRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (!vm.address_editing || vm.address_searching) {
            std::vector<BreadcrumbPlaced> placed;
            BreadcrumbLayout(vm.pane, rect.right, placed);
            for (size_t i = 0; i < placed.size(); ++i) {
                const auto& seg = placed[i];
                if ((int)i == vm.breadcrumb_drop) {
                    // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                    dc->DrawRoundedRectangle(
                        D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                        brAccent_.get(), 2.0f * scale_);
                } else if ((int)i == vm.breadcrumb_hover) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top,
                        theme.radius_control * scale_);
                }
                if (i > 0) {
                    // Chevron separator.
                    float chX = seg.rc.left - 14.0f * scale_;
                    DrawIconText(chX, addrRc.top, 14.0f * scale_, addrRc.bottom - addrRc.top,
                        kIconChevronRight, L">", theme.text_secondary, 0.55f);
                }
                MakeBrush(dc, theme.text, brText_);
                // Width measured exactly; let the ink use the right padding as slack
                // so the trailing glyph is not shaved by the clip rect.
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), seg.text,
                    seg.rc.left + 8 * scale_, seg.rc.top, seg.rc.right - seg.rc.left - 8 * scale_,
                    seg.rc.bottom - seg.rc.top);
            }
            if (placed.empty() && !vm.pane.path.empty()) {
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.pane.path,
                    addrRc.left + 12.0f * scale_, addrRc.top,
                    addrRc.right - addrRc.left - 18.0f * scale_,
                    addrRc.bottom - addrRc.top);
            }
        }
        dc->PopAxisAlignedClip();
    }
    DrawAddressSearchChrome(vm, rect.right, theme);
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),left+margin_,title_bar_height_+44*scale_,rect.right-left-2*margin_,scale_);
    const std::wstring label=l10n::Get(l10n::StringId::New);
    fluent::ButtonSpec create;
    create.bounds=layout.create; create.text=compact ? std::wstring_view{} : std::wstring_view{label};
    create.glyph=kIconAdd; create.kind=fluent::ButtonKind::Primary;
    create.icon_only=compact; create.drop_down=!compact;
    create.state.hovered=IsHovered(vm,HitTestResult::NewButton);
    painter_.DrawButton(create);
    const wchar_t* glyphs[]={kIconCut,kIconCopy,kIconPaste,kIconRename,kIconDelete,kIconSplit,
        vm.details_visible ? kIconDetailsClose : kIconDetailsOpen,L""};
    const HitTestResult::Region hits[]={HitTestResult::Cut,HitTestResult::Copy,HitTestResult::Paste,
        HitTestResult::Rename,HitTestResult::Delete,HitTestResult::SplitButton,HitTestResult::DetailsToggle,HitTestResult::PaneColumnLayout};
    for(int i=0;i<8;++i) {
        if (layout.commands[i].right <= layout.commands[i].left) continue;
        const bool enabled=i==2 || i>=5 || vm.pane.selected_count>0;
        const bool selected=(i==5 && vm.pane_slots.size()>1) || (i==6 && vm.details_visible) ||
            (i==7 && vm.pane.column_strip.enabled);
        DrawButton(layout.commands[i],theme,enabled && IsHovered(vm,hits[i]) ? theme.fill_hover :
            selected ? theme.fill_selected : kTransparent,
            i==5 ? L"" : glyphs[i],L"",!enabled ? theme.text_disabled : selected ? theme.accent : theme.text_secondary,true,true);
        if (i==5) DrawPaneHeaderIcon(layout.commands[i],PaneHeaderIcon::Split,selected ? theme.accent : theme.text_secondary);
        if (i==7) DrawPaneHeaderIcon(layout.commands[i],PaneHeaderIcon::Columns,selected ? theme.accent : theme.text_secondary);
    }
    if (layout.overflow.right > layout.overflow.left) {
        DrawButton(layout.overflow,theme,IsHovered(vm,HitTestResult::ToolbarMore) ? theme.fill_hover : kTransparent,
            L"\xE712",L"",theme.text_secondary,true,true);
    }
    const auto command = [&](D2D1_RECT_F bounds, const wchar_t* glyph, l10n::StringId label,
                             HitTestResult::Region region, bool dropdown) {
        fluent::ButtonSpec button;
        button.bounds=bounds;
        button.glyph=glyph;
        button.icon_only=bounds.right-bounds.left < 60*scale_;
        button.text=button.icon_only ? std::wstring_view{} : std::wstring_view{l10n::Get(label)};
        button.kind=fluent::ButtonKind::Transparent;
        button.bordered=false;
        button.drop_down=dropdown && !button.icon_only;
        button.state.hovered=IsHovered(vm,region);
        painter_.DrawButton(button);
    };
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),layout.sort.left-6*scale_,layout.sort.top+7*scale_,scale_,18*scale_);
    command(layout.sort,L"\xE8CB",l10n::StringId::ToolbarSort,HitTestResult::ToolbarSort,true);
    if (vm.pane.filter_expand <= 0.015f && !vm.filter_editing) {
        command(layout.filter,kIconFilter,l10n::StringId::ToolbarFilter,HitTestResult::FilterBox,false);
    } else {
        fluent::TextFieldSpec field;
        field.bounds=layout.filter;
        field.placeholder=l10n::Get(l10n::StringId::FilterPlaceholder);
        field.text=vm.pane.filter_text;
        field.leading_glyph=kIconFilter;
        field.compact_leading_glyph=true;
        field.suppress_text=vm.filter_editing;
        field.state.focused=vm.filter_editing;
        field.state.hovered=IsHovered(vm,HitTestResult::FilterBox);
        if (!vm.pane.filter_text.empty()) field.trailing_width=30;
        painter_.DrawTextField(field);
        if (!vm.pane.filter_text.empty() && vm.pane.filter_expand >= 0.985f) {
            DrawButton(FilterClearRect(rect,vm.pane.filter_expand),theme,
                IsHovered(vm,HitTestResult::FilterClear) ? theme.fill_hover : kTransparent,
                L"\xE711",L"",theme.text_secondary,true,true,0.65f);
        }
    }
}
}
