#include "../ui/math_layout.h"
#include "../ui/math_formula.h"
#include <cmath>
#include <cstdio>
#include <limits>

using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
bool Close(float a, float b) { return std::abs(a - b) < 0.15f; }
}
int wmain() {
    using namespace pulse::ui;
    using namespace pulse::ui::math;
    ComPtr<IDWriteFactory2> factory;
    Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf()))), "real DirectWrite factory");
    if (!factory) return 1;
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    UINT32 index = 0;
    BOOL exists = FALSE;
    const bool found = SUCCEEDED(factory->GetSystemFontCollection(&collection)) &&
        SUCCEEDED(collection->FindFamilyName(L"Cambria Math", &index, &exists)) && exists &&
        SUCCEEDED(collection->GetFontFamily(index, &family)) &&
        SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, &font)) && SUCCEEDED(font->CreateFontFace(&face));
    Check(found, "actual mathematical font loaded");
    if (!found) return 1;
    const auto metrics = ReadFontMathMetrics(face.Get());
    auto build = [&](std::wstring_view source, float scale, bool display, FormulaLayout& layout) {
        const auto syntax = ParseMathSyntax(source);
        return syntax && BuildMathLayout(factory.Get(), face.Get(), metrics, syntax.root, 24 * scale, display, layout, scale);
    };
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (bool display : {false, true}) {
            FormulaLayout base, pt, em, ex, script_base, script_pt, row_base, row_pt;
            Check(build(L"ab", scale, display, base) && build(LR"(a\hspace{18pt}b)", scale, display, pt) &&
                Close((pt.width - base.width) / scale, 18 * 96.0f / 72.27f),
                "absolute pt spacing follows independent window DPI");
            Check(build(LR"(a\hspace{1em}b)", scale, display, em) && Close((em.width - base.width) / scale, 24),
                "em spacing is not scaled twice");
            DWRITE_FONT_METRICS fm{};
            face->GetMetrics(&fm);
            Check(build(LR"(a\hspace{1ex}b)", scale, display, ex) &&
                Close((ex.width - base.width) / scale, 24.0f * fm.xHeight / fm.designUnitsPerEm),
                "ex spacing is not scaled twice");
            Check(build(L"x^{ab}", scale, display, script_base) &&
                build(LR"(x^{a\hspace{18pt}b})", scale, display, script_pt) &&
                Close((script_pt.width - script_base.width) / scale, 18 * 96.0f / 72.27f),
                "absolute pt spacing does not shrink with script style");
            Check(build(LR"(\begin{array}{c}a\\b\end{array})", scale, display, row_base) &&
                build(LR"(\begin{array}{c}a\\[18pt]b\end{array})", scale, display, row_pt) &&
                Close((row_pt.ascent + row_pt.descent - row_base.ascent - row_base.descent) / scale,
                    18 * 96.0f / 72.27f), "absolute array row gap follows DPI");
        }
        for (const auto source : {LR"(a\hspace{-6pt}b)", LR"(x^{a\hspace{6pt}b})", LR"(\frac{a}{b}\hspace{18pt}x)"}) {
            FormulaLayout baseline, scaled;
            Check(build(source, 1, false, baseline) && build(source, scale, false, scaled) &&
                Close(baseline.width, scaled.width / scale) && Close(baseline.ascent, scaled.ascent / scale) &&
                Close(baseline.descent, scaled.descent / scale), "signed and nested geometry remains invariant in logical units");
        }
        const std::wstring source = LR"(a\hspace{18pt}b)";
        ComPtr<IDWriteTextFormat> format;
        Check(SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24 * scale, L"en-US", &format)), "inline text format created");
        for (float width : {10000.0f, 60.0f}) {
            ComPtr<IDWriteTextLayout> layout;
            ComPtr<IDWriteInlineObject> object;
            DWRITE_INLINE_OBJECT_METRICS m{};
            const bool ok = SUCCEEDED(factory->CreateTextLayout(source.data(), static_cast<UINT32>(source.size()),
                format.Get(), width * scale, 1000 * scale, &layout)) &&
                ApplyMathInline(factory.Get(), layout.Get(), {0, static_cast<UINT32>(source.size())},
                    source, 24 * scale, false, width * scale, scale) &&
                SUCCEEDED(layout->GetInlineObject(0, &object)) && SUCCEEDED(object->GetMetrics(&m));
            Check(ok && m.width > 0 && m.width <= width * scale + 0.1f && m.height > 0,
                "real inline object preserves fit bounds at window DPI");
        }
    }
    const auto syntax = ParseMathSyntax(LR"(a\hspace{18pt}b)");
    FormulaLayout invalid;
    Check(!BuildMathLayout(factory.Get(), face.Get(), metrics, syntax.root, 24, false, invalid, 0) &&
        !BuildMathLayout(factory.Get(), face.Get(), metrics, syntax.root, 24, false, invalid,
            std::numeric_limits<float>::quiet_NaN()), "invalid window scales rejected");
    return failures ? 1 : 0;
}
