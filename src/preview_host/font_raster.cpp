// font_raster.cpp — see font_raster.h.
#include "font_raster.h"

#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace pulse::preview {
namespace {

using Microsoft::WRL::ComPtr;

using D2D1CreateFactoryFn = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID,
                                              const D2D1_FACTORY_OPTIONS*, void**);
using DWriteCreateFactoryFn = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

// Resolved on first use so the host does not import d2d1/dwrite at start-up.
template <typename Fn>
Fn Resolve(const wchar_t* dll, const char* name) {
    const HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

IDWriteFactory5* DWrite() {
    static ComPtr<IDWriteFactory5> factory = [] {
        ComPtr<IDWriteFactory5> f;
        static const auto create = Resolve<DWriteCreateFactoryFn>(L"dwrite.dll", "DWriteCreateFactory");
        if (create) create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                           reinterpret_cast<IUnknown**>(f.GetAddressOf()));
        return f;
    }();
    return factory.Get();
}

ID2D1Factory* D2D() {
    static ComPtr<ID2D1Factory> factory = [] {
        ComPtr<ID2D1Factory> f;
        static const auto create = Resolve<D2D1CreateFactoryFn>(L"d2d1.dll", "D2D1CreateFactory");
        D2D1_FACTORY_OPTIONS options{};
        if (create) create(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options,
                           reinterpret_cast<void**>(f.GetAddressOf()));
        return f;
    }();
    return factory.Get();
}

std::wstring Localized(IDWriteLocalizedStrings* strings) {
    if (!strings || strings->GetCount() == 0) return {};
    UINT32 index = 0;
    BOOL exists = FALSE;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH))
        strings->FindLocaleName(locale, &index, &exists);
    if (!exists) strings->FindLocaleName(L"en-us", &index, &exists);
    if (!exists) index = 0;
    UINT32 length = 0;
    if (FAILED(strings->GetStringLength(index, &length))) return {};
    std::wstring out(length + 1, L'\0');
    if (FAILED(strings->GetString(index, out.data(), length + 1))) return {};
    out.resize(length);
    return out;
}

bool Covers(IDWriteFont* font, std::wstring_view text) {
    for (wchar_t c : text) {
        if (c == L' ') continue;
        BOOL has = FALSE;
        if (FAILED(font->HasCharacter(c, &has)) || !has) return false;
    }
    return true;
}

} // namespace

bool RasterizeFontFile(const std::wstring& path, UINT max_edge,
                       std::vector<unsigned char>& pixels, UINT& width, UINT& height,
                       UINT& stride, std::wstring* error) {
    auto fail = [&](const wchar_t* why) { if (error) *error = why; return false; };
    IDWriteFactory5* dwrite = DWrite();
    ID2D1Factory* d2d = D2D();
    if (!dwrite || !d2d) return fail(L"font-no-dwrite");

    // Load the file into a private collection (no install, no registry).
    ComPtr<IDWriteFontFile> file;
    ComPtr<IDWriteFontSetBuilder1> builder;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(dwrite->CreateFontFileReference(path.c_str(), nullptr, &file)))
        return fail(L"font-open-failed");
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type{};
    DWRITE_FONT_FACE_TYPE face_type{};
    UINT32 faces = 0;
    if (FAILED(file->Analyze(&supported, &file_type, &face_type, &faces)) || !supported || !faces)
        return fail(L"font-unsupported");
    if (FAILED(dwrite->CreateFontSetBuilder(&builder)) || FAILED(builder->AddFontFile(file.Get())) ||
        FAILED(builder->CreateFontSet(&set)) ||
        FAILED(dwrite->CreateFontCollectionFromFontSet(set.Get(), &collection)) ||
        collection->GetFontFamilyCount() == 0)
        return fail(L"font-load-failed");

    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteLocalizedStrings> family_names, face_names;
    if (FAILED(collection->GetFontFamily(0, &family)) || FAILED(family->GetFont(0, &font)))
        return fail(L"font-load-failed");
    family->GetFamilyNames(&family_names);
    font->GetFaceNames(&face_names);
    const std::wstring family_name = Localized(family_names.Get());
    const std::wstring face_name = Localized(face_names.Get());
    ComPtr<IDWriteFontFace> face;
    font->CreateFontFace(&face);
    const UINT32 glyphs = face ? face->GetGlyphCount() : 0;
    // Line advance from the font's own metrics: tall CJK/CFF designs (Source
    // Han, etc.) need more than a fixed 1.35 em or their descenders clip.
    float line_factor = 1.35f;
    if (face) {
        DWRITE_FONT_METRICS fm{};
        face->GetMetrics(&fm);
        if (fm.designUnitsPerEm)
            line_factor = (std::clamp)(static_cast<float>(fm.ascent + fm.descent + fm.lineGap) /
                                       fm.designUnitsPerEm * 1.05f, 1.1f, 1.9f);
    }
    const DWRITE_FONT_WEIGHT weight = font->GetWeight();
    const DWRITE_FONT_STYLE style = font->GetStyle();
    const DWRITE_FONT_STRETCH stretch = font->GetStretch();

    // Samples the font can actually show.
    const bool latin = Covers(font.Get(), L"AaBbQq09");
    const bool cjk = Covers(font.Get(), L"\x6C38\x5929\x5730");
    std::wstring lines[3];
    if (latin) {
        lines[0] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        lines[1] = L"abcdefghijklmnopqrstuvwxyz";
        lines[2] = L"0123456789 !?&@#%$()";
    }
    const std::wstring cjk_line = L"\x5929\x5730\x7384\x9EC4 \x5B87\x5B99\x6D2A\x8352 \x6C38\x548C\x4E5D\x5E74";
    const std::wstring waterfall = latin ? L"The quick brown fox jumps over the lazy dog"
                                         : (cjk ? L"\x6C38\x548C\x4E5D\x5E74\xFF0C\x5C81\x5728\x7678\x4E11" : L"");
    if (!latin && !cjk) {
        // Symbol fonts: show whatever the first mapped code points draw.
        std::wstring any;
        for (wchar_t c = 0x21; c < 0x7F && any.size() < 26; ++c) {
            BOOL has = FALSE;
            if (SUCCEEDED(font->HasCharacter(c, &has)) && has) any.push_back(c);
        }
        if (any.empty())
            for (wchar_t c = 0xF021; c < 0xF07F && any.size() < 26; ++c) any.push_back(c);
        lines[0] = any;
    }

    const bool cff = file_type == DWRITE_FONT_FILE_TYPE_CFF || face_type == DWRITE_FONT_FACE_TYPE_CFF;
    // The host has no string table; follow the Windows display language.
    const bool zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    std::wstring meta = cff ? L"OpenType" : L"TrueType";
    if (!face_name.empty()) meta += L" \x00B7 " + face_name;
    if (glyphs) meta += L" \x00B7 " + std::to_wstring(glyphs) + (zh ? L" \x4E2A\x5B57\x5F62" : L" glyphs");
    if (faces > 1) meta += L" \x00B7 " + std::to_wstring(faces) + (zh ? L" \x79CD\x5B57\x4F53" : L" faces");

    // Page: 4:3, longest edge max_edge.
    const UINT W = (std::clamp)(max_edge, 128u, 2048u);
    const UINT H = W * 3 / 4;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
        return fail(L"font-target-failed");
    ComPtr<ID2D1RenderTarget> rt;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt)))
        return fail(L"font-target-failed");
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    ComPtr<ID2D1SolidColorBrush> ink, soft, rule;
    rt->CreateSolidColorBrush(D2D1::ColorF(0x111827), &ink);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x6B7280), &soft);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xE5E7EB), &rule);

    const float w = static_cast<float>(W), h = static_cast<float>(H);
    const float margin = w * 0.06f;
    const float right = w - margin;
    auto format = [&](IDWriteFontCollection* coll, const wchar_t* name, float size,
                      DWRITE_FONT_WEIGHT fw, DWRITE_FONT_STYLE fs, DWRITE_FONT_STRETCH fst) {
        ComPtr<IDWriteTextFormat> f;
        if (SUCCEEDED(dwrite->CreateTextFormat(name, coll, fw, fs, fst, size, L"", &f))) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            f->SetTrimming(&trimming, nullptr);
        }
        return f;
    };
    auto draw = [&](IDWriteTextFormat* f, const std::wstring& text, float top, float bottom,
                    ID2D1Brush* brush) {
        if (!f || text.empty()) return;
        // No per-line clip: the page clip below trims only the side margin.
        rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), f,
                     D2D1::RectF(margin, top, right, bottom), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    };
    IDWriteFontCollection* own = collection.Get();
    const wchar_t* fam = family_name.c_str();

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0xFFFFFF));
    rt->PushAxisAlignedClip(D2D1::RectF(margin, 0.0f, right, h), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = margin * 0.8f;
    const float title = h * 0.10f;
    draw(format(own, fam, title, weight, style, stretch).Get(), family_name.empty() ? L"Aa" : family_name,
         y, y + title * line_factor, ink.Get());
    y += title * line_factor;
    const float meta_px = h * 0.030f;
    draw(format(nullptr, L"Segoe UI", meta_px, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL).Get(), meta, y, y + meta_px * 1.5f, soft.Get());
    y += meta_px * 1.5f + h * 0.02f;
    rt->FillRectangle(D2D1::RectF(margin, y, right, y + (std::max)(1.0f, h * 0.002f)), rule.Get());
    y += h * 0.035f;

    const float body = h * 0.052f;
    auto body_format = format(own, fam, body, weight, style, stretch);
    for (const auto& line : lines) {
        if (line.empty()) continue;
        draw(body_format.Get(), line, y, y + body * line_factor, ink.Get());
        y += body * line_factor;
    }
    if (cjk) {
        draw(body_format.Get(), cjk_line, y, y + body * line_factor, ink.Get());
        y += body * line_factor;
    }
    y += h * 0.025f;
    const float sizes[] = {0.075f, 0.052f, 0.036f, 0.026f};
    for (float s : sizes) {
        const float px = h * s;
        if (y + px * line_factor > h - margin * 0.4f) break;
        draw(format(own, fam, px, weight, style, stretch).Get(), waterfall, y, y + px * line_factor, ink.Get());
        y += px * line_factor;
    }
    rt->PopAxisAlignedClip();
    if (FAILED(rt->EndDraw())) return fail(L"font-draw-failed");
    rt.Reset();

    stride = W * 4;
    pixels.resize(static_cast<size_t>(stride) * H);
    if (FAILED(bitmap->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
        return fail(L"font-readback-failed");
    width = W;
    height = H;
    return true;
}

} // namespace pulse::preview
