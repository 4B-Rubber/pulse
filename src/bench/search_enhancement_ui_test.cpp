#include "../ui/name_highlight.h"
#include "../ui/address_search_layout.h"
#include "../app/search_query.h"
#include <array>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
namespace {
int failures = 0;
void Check(bool pass, const char* label) {
    std::cout << (pass ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!pass) ++failures;
}
void Ranges(std::wstring_view name, std::wstring_view query,
            std::initializer_list<std::pair<UINT32, UINT32>> expected, const char* label) {
    const auto terms = pulse::ui::NameHighlightTerms({}, query);
    const auto ranges = pulse::ui::NameMatchRanges(name, terms);
    bool same = ranges.size() == expected.size();
    size_t i = 0;
    for (const auto& [start, length] : expected) {
        if (i >= ranges.size() || ranges[i].start != start || ranges[i].length != length) same = false;
        ++i;
    }
    if (!same) {
        std::cout << "[INFO] actual spans:";
        for (const auto& range : ranges) std::cout << ' ' << range.start << '+' << range.length;
        std::cout << '\n';
    }
    Check(same, label);
}
bool RectInside(D2D1_RECT_F rect, D2D1_RECT_F field) {
    constexpr float epsilon = 0.01f;
    return std::isfinite(rect.left) && std::isfinite(rect.right) && std::isfinite(rect.top) && std::isfinite(rect.bottom) &&
        rect.left + epsilon >= field.left && rect.right <= field.right + epsilon &&
        rect.top + epsilon >= field.top && rect.bottom <= field.bottom + epsilon &&
        rect.right + epsilon >= rect.left && rect.bottom + epsilon >= rect.top;
}
bool ValidSlots(const pulse::ui::AddressSearchLayout& layout, D2D1_RECT_F field) {
    const std::array<D2D1_RECT_F, 5> ordered = {
        layout.scope, layout.input, layout.mode, layout.options, layout.close};
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (!RectInside(ordered[i], field) ||
            (i && ordered[i - 1].right > ordered[i].left + 0.01f)) return false;
    }
    // Rendering uses mode; hit testing uses its name alias. Content and clear
    // are intentionally empty because one mode chip and one trailing x are used.
    return RectInside(layout.name, field) && RectInside(layout.content, field) &&
        RectInside(layout.clear, field) &&
        layout.name.left == layout.mode.left && layout.name.right == layout.mode.right &&
        layout.name.top == layout.mode.top && layout.name.bottom == layout.mode.bottom &&
        layout.content.left == layout.mode.right && layout.content.right == layout.mode.right &&
        layout.clear.left == layout.close.left && layout.clear.right == layout.close.left;
}
void LayoutCase(float width, float scale, bool logical_width) {
    const float physical_width = logical_width ? width * scale : width;
    const auto field = D2D1::RectF(17.0f, 9.0f, 17.0f + physical_width, 9.0f + 36.0f * scale);
    const auto layout = pulse::ui::LayoutAddressSearch(field, scale);
    const bool slots_valid = ValidSlots(layout, field);
    const float field_dip = physical_width / scale;
    const bool responsive = layout.scope_label == (field_dip >= 280) &&
        layout.mode_label == (field_dip >= 400) &&
        (layout.scope.right > layout.scope.left) == (field_dip >= 200) &&
        (layout.options.right > layout.options.left) == (field_dip >= 260) &&
        (layout.close.right > layout.close.left) == (field_dip >= 140);
    const float input_width = (layout.input.right - layout.input.left) / scale;
    // Normal pane sizes keep at least 64 DIP; constrained physical widths must
    // still leave a usable 32 DIP edit region instead of negative geometry.
    const bool enough_input = input_width + 0.01f >= (logical_width ? 64.0f : 32.0f);
    if (!slots_valid || !responsive || !enough_input)
        std::cout << "[INFO] width=" << width << " scale=" << scale << " logical=" << logical_width
                  << " inputDIP=" << input_width << " slots=" << slots_valid << " responsive=" << responsive << '\n';
    Check(slots_valid && responsive && enough_input, logical_width ? "DIP-scaled address search bounds/input" : "physical-width address search bounds/input");
}
}
int main() {
    using namespace pulse;
    Ranges(L"报告-中国.txt", L"zhongguo", {{3, 2}}, "full-pinyin highlights original Chinese characters");
    Ranges(L"中国报告.txt", L"zgbg", {{0, 4}}, "initials highlight original character span");
    Ranges(L"重庆音乐.txt", L"chongqing", {{0, 2}}, "polyphonic full-pinyin highlight");
    Ranges(L"zg-中国.txt", L"zg", {{0, 2}}, "literal occurrence takes precedence over initials");
    Ranges(L"中国-zhongguo.txt", L"zhongguo", {{3, 8}}, "literal occurrence takes precedence over full pinyin");
    Ranges(L"中国.txt", L"zhongguo nopinyin:", {}, "nopinyin disables Chinese transliteration highlight");
    Ranges(L"zhongguo-中国.txt", L"zhongguo nopinyin:", {{0, 8}}, "nopinyin preserves literal highlighting");
    Ranges(L"foo-文档Report2026.txt", L"wendangreport2026", {{4, 12}}, "mixed pinyin English digits span");
    Ranges(L"中国报告.txt", L"中国baogao", {{0, 4}}, "mixed literal Chinese and pinyin span");
    Ranges(L"中国.txt", L"!zhongguo", {}, "excluded terms are not highlighted");
    Ranges(L"中国.txt", L"path:zhongguo", {}, "path terms are not filename highlights");
    Ranges(L"中国.txt", L"\"zhongguo\"", {}, "quoted exact term stays literal");
    Ranges(L"中国.txt", L"z", {}, "one Latin letter does not highlight pinyin");
    const auto pane_terms = ui::NameHighlightTerms(L"zgbg", {});
    Check(ui::NameMatchRanges(L"中国报告.txt", pane_terms).empty(), "pane filter keeps its literal semantics");
    const auto original_ranges = ui::NameMatchRanges(L"中国报告2026.txt", ui::NameHighlightTerms({}, L"zgbg"));
    const auto visible = ui::VisibleNameMatchRanges(L"中国报告2026.txt", L"中国…txt", original_ranges);
    Check(visible.size() == 1 && visible[0].start == 0 && visible[0].length == 2,
          "truncated labels highlight visible original characters only");
    for (const auto raw : {L"zhongguo nopinyin:", L"nopinyin: zgbg ext:txt", L"zhongguo content:budget nopinyin:"}) {
        const auto restored = app::CompileSearchQuery(app::ParseSearchQuery(raw));
        const auto compiled = index::ParseQuery(restored);
        Check(!compiled.pinyin_enabled && !index::QueryHasPinyin(compiled), "advanced/saved query roundtrip preserves nopinyin");
        const auto split = app::SplitSearchQueryText(raw);
        Check(!index::ParseQuery(split.filename_needle).pinyin_enabled, "content filename split preserves nopinyin");
    }
    const auto enabled = app::CompileSearchQuery(app::ParseSearchQuery(L"zhongguo"));
    Check(index::QueryHasPinyin(index::ParseQuery(enabled)), "advanced query roundtrip preserves default pinyin");
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (float width : {139.0f, 140.0f, 180.0f, 199.0f, 200.0f, 259.0f, 260.0f,
                            279.0f, 280.0f, 339.0f, 340.0f, 399.0f, 400.0f, 520.0f}) {
            LayoutCase(width, scale, true);
            LayoutCase(width, scale, false);
        }
    }
    const auto field = D2D1::RectF(17.0f, 9.0f, 537.0f, 45.0f);
    auto invalid = ui::LayoutAddressSearch(field, 1.0f);
    invalid.mode.left = invalid.input.right - 1.0f;
    invalid.name = invalid.mode;
    Check(!ValidSlots(invalid, field), "layout guard rejects actual input/mode overlap");
    invalid = ui::LayoutAddressSearch(field, 1.0f);
    invalid.name.left += 1.0f;
    Check(!ValidSlots(invalid, field), "layout guard rejects drawing/hit-target disagreement");
    return failures ? 1 : 0;
}
