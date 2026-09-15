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
void LayoutCase(float width, float scale, bool logical_width) {
    const float physical_width = logical_width ? width * scale : width;
    const auto field = D2D1::RectF(17.0f, 9.0f, 17.0f + physical_width, 9.0f + 36.0f * scale);
    const auto layout = pulse::ui::LayoutAddressSearch(field, scale);
    const std::array<D2D1_RECT_F, 7> ordered = {layout.scope, layout.name, layout.content, layout.input, layout.clear, layout.options, layout.close};
    bool inside = true, separated = true;
    for (size_t i = 0; i < ordered.size(); ++i) {
        inside &= RectInside(ordered[i], field);
        if (i) separated &= ordered[i - 1].right <= ordered[i].left + 0.01f;
    }
    const float input_width = (layout.input.right - layout.input.left) / scale;
    // Normal pane sizes keep at least 64 DIP; constrained physical widths must
    // still leave a usable 32 DIP edit region instead of negative geometry.
    const bool enough_input = input_width + 0.01f >= (logical_width ? 64.0f : 32.0f);
    if (!inside || !separated || !enough_input)
        std::cout << "[INFO] width=" << width << " scale=" << scale << " logical=" << logical_width
                  << " inputDIP=" << input_width << " inside=" << inside << " separated=" << separated << '\n';
    Check(inside && separated && enough_input, logical_width ? "DIP-scaled address search bounds/input" : "physical-width address search bounds/input");
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
        for (float width : {180.0f, 260.0f, 340.0f, 520.0f}) {
            LayoutCase(width, scale, true);
            LayoutCase(width, scale, false);
        }
    }
    return failures ? 1 : 0;
}
