#include "ui_renderer.h"
#include "ui_renderer_internal.h"

namespace pulse::ui {
void MainRenderer::DrawSettingsContext(const WindowViewModel& vm,const D2D1_RECT_F& rect,const Theme& theme) {
    using I=l10n::StringId;
    auto* dc=compositor_->Dc();
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    painter_.DrawText(l10n::Get(I::ContextMenuDesc),D2D1::RectF(l.content.left+20*scale_,l.content_origin+64*scale_,l.content.right-20*scale_,l.content_origin+86*scale_),compositor_->SmallFormat(),theme.text_secondary);
    const I titles[]={I::ContextSoftware,I::ContextOpenWith,I::ContextShare,I::ContextSystem,I::ContextPrint};
    const I descriptions[]={I::ContextSoftwareDesc,I::ContextOpenWithDesc,I::ContextShareDesc,I::ContextSystemDesc,I::ContextPrintDesc};
    const wchar_t* icons[]={L"\xE74C",L"\xE8A7",L"\xE72D",L"\xE713",L"\xE749"};
    for(int g=0;g<5;++g) {
        const auto r=l.context_cards[g],header=l.context_header[g];
        MakeBrush(dc,theme.fill_input,brFillInput_);MakeBrush(dc,theme.stroke_card,brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brStrokeCard_.get(),1);
        if(IsHovered(vm,HitTestResult::SettingsDisclosure,8+g)) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(header,8*scale_,8*scale_),brFillHover_.get());
        }
        DrawIconText(r.left+16*scale_,r.top+20*scale_,24*scale_,24*scale_,icons[g],L"",theme.text_secondary,0.85f);
        painter_.DrawText(l10n::Get(titles[g]),D2D1::RectF(r.left+54*scale_,r.top+12*scale_,l.context_toggle[g].left-12*scale_,r.top+36*scale_),compositor_->TextFormat(),theme.text);
        painter_.DrawText(l10n::Get(descriptions[g]),D2D1::RectF(r.left+54*scale_,r.top+39*scale_,l.context_toggle[g].left-12*scale_,r.top+65*scale_),compositor_->SmallFormat(),theme.text_secondary);
        fluent::ControlState state{};state.checked=vm.settings_group_on[g];state.hovered=IsHovered(vm,HitTestResult::SettingsToggle,10+g);
        painter_.DrawSwitch(l.context_toggle[g],L"",state);
        DrawIconText(r.right-36*scale_,r.top+24*scale_,18*scale_,18*scale_,(vm.settings_expanded&(1u<<(8+g))) ? L"\xE70D" : L"\xE76C",L"",theme.text_secondary,0.75f);
    }
    for(const auto& empty:l.context_empty) if(empty.bottom>empty.top)
        painter_.DrawText(l10n::Get(I::SettingsContextEmpty),empty,compositor_->SmallFormat(),theme.text_secondary);
    for(size_t i=0;i<l.context_rows.size();++i) {
        const auto r=l.context_rows[i];if(r.bottom<=r.top) continue;
        if(IsHovered(vm,HitTestResult::SettingsToggle,100+static_cast<int>(i))) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(r,4*scale_,4*scale_),brFillHover_.get());
        }
        painter_.DrawText(vm.settings_items[i].text,D2D1::RectF(r.left+16*scale_,r.top,r.right-76*scale_,r.bottom),compositor_->TextFormat(),theme.text);
        fluent::ControlState state{};state.checked=vm.settings_items[i].on;state.hovered=IsHovered(vm,HitTestResult::SettingsToggle,100+static_cast<int>(i));
        painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+4*scale_,r.right-16*scale_,r.bottom-4*scale_),L"",state);
    }
    fluent::ControlState restore{};restore.hovered=IsHovered(vm,HitTestResult::SettingsRestore);
    painter_.DrawButton({l.context_restore,l10n::Get(I::RestoreDefaults),L"",fluent::ButtonKind::Standard,restore});
}
}
