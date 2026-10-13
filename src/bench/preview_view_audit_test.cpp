#include "../ui/markdown_view.h"
#include "../ui/math_layout.h"
#include "../ui/tree_view.h"
#include "../ui/table_view.h"
#include "../preview_host/tree_document.h"
#include <d3d11.h>
#include <objbase.h>
#include <xmllite.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cmath>
using Microsoft::WRL::ComPtr;
int wmain() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IDWriteFactory2> factory;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    ComPtr<ID3D11Device> d3d;
    if (SUCCEEDED(hr)) hr = D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d3d,nullptr,nullptr);
    ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(hr)) hr = d3d.As(&dxgi);
    ComPtr<ID2D1Device> device;
    if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.Get(),nullptr,&device);
    ComPtr<ID2D1DeviceContext> dc;
    if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc);
    ComPtr<ID2D1Bitmap1> bitmap;
    if (SUCCEEDED(hr)) hr = dc->CreateBitmap(D2D1::SizeU(800,600),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&bitmap);
    check(SUCCEEDED(hr),"create offscreen WARP drawing target");
    if (FAILED(hr)) return 1;
    dc->SetTarget(bitmap.Get());
    pulse::ui::MarkdownView view;
    std::wstring payload = L"PULSEMD\t1\nT\t20\t0\t0\nR\n";
    for(int c=0;c<20;++c) payload += L"B\tt\t\t0\t0\t\tcell" + std::to_wstring(c) + L"\t\n";
    payload += L"E\n";
    check(view.SetPayload(payload,L"C:\\fixture.md"),"parse wide Markdown table");
    auto draw = [&] { dc->BeginDraw(); view.Draw(dc.Get(),factory.Get(),D2D1::RectF(0,0,300,300),pulse::ui::MakeTheme(false,D2D1::ColorF(0.2f,0.4f,0.8f)),false,1,nullptr,1,0,0,{}); return SUCCEEDED(dc->EndDraw()); };
    check(draw() && view.ScrollHorizontal(-100),"wide table has reachable horizontal range");
    const auto last = static_cast<uint32_t>(view.PlainText().find(L"cell19"));
    view.Reveal(last);
    check(draw(),"find reveal lays out last Markdown column");
    bool found = false;
    for (int y=0;y<200 && !found;++y) for(int x=0;x<300;++x) { uint32_t offset=0; if(view.HitTest(static_cast<float>(x),static_cast<float>(y),offset) && offset>=last && offset<last+6) {found=true;break;} }
    check(found,"last-column text remains reachable through production hit testing after reveal");
    check(view.SetPayload(L"PULSEMD\t1\nB\tp\t\t0\t0\t\t\U0001f600A e\u0301\t\n",L"C:\\unicode.md") && draw(),"layout surrogate and combining clusters");
    bool split_cluster=false;
    for(int y=0;y<100;++y) for(int x=0;x<300;++x) {
        uint32_t offset=0;
        if(view.HitTest(static_cast<float>(x),static_cast<float>(y),offset) && (offset==1 || offset==5)) split_cluster=true;
    }
    check(!split_cluster,"production Markdown hit testing never splits UTF-16 or combining clusters");
    pulse::ui::TableView table;
    check(table.SetPayload(L"PULSETBL\t1\nS\tcsv\tHeader only\t1\t1\t0\tcomma\t1\nR\tNeedle\n"),"parse header-only CSV");
    ComPtr<ID2D1Bitmap1> readback;
    hr=dc->CreateBitmap(D2D1::SizeU(800,600),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&readback);
    auto table_hash=[&](bool match) {
        dc->BeginDraw();dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
        table.Draw(dc.Get(),factory.Get(),D2D1::RectF(0,0,300,300),pulse::ui::MakeTheme(false,D2D1::ColorF(0.2f,0.4f,0.8f)),false,1,D2D1::ColorF(D2D1::ColorF::White),match?std::vector<pulse::ui::TableView::Highlight>{{0,6,true}}:std::vector<pulse::ui::TableView::Highlight>{});
        if(FAILED(dc->EndDraw())||!readback||FAILED(readback->CopyFromBitmap(nullptr,bitmap.Get(),nullptr)))return uint64_t{0};
        D2D1_MAPPED_RECT mapped{};if(FAILED(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped)))return uint64_t{0};
        uint64_t hash=1469598103934665603ull;
        for(UINT y=0;y<300;++y)for(UINT x=0;x<1200;++x){hash^=mapped.bits[y*mapped.pitch+x];hash*=1099511628211ull;}
        readback->Unmap();return hash;
    };
    const auto plain_header=table_hash(false),matched_header=table_hash(true);
    check(plain_header && matched_header && plain_header!=matched_header,"header-only CSV find match visibly changes pinned-header pixels");
    ComPtr<IDWriteFontCollection> fonts; ComPtr<IDWriteFontFamily> family; ComPtr<IDWriteFont> font; ComPtr<IDWriteFontFace> face;
    UINT32 index=0; BOOL exists=FALSE;
    factory->GetSystemFontCollection(&fonts); fonts->FindFamilyName(L"Cambria Math",&index,&exists);
    if(exists) { fonts->GetFontFamily(index,&family); family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&font); font->CreateFontFace(&face); }
    check(face != nullptr,"Cambria Math available for physical spacing regression");
    if(face) {
        auto syntax=pulse::ui::ParseMathSyntax(L"a\\hspace{10pt}b");
        auto baseline=pulse::ui::ParseMathSyntax(L"ab");
        auto metrics=pulse::ui::math::ReadFontMathMetrics(face.Get());
        for(float scale: {1.0f,1.5f,2.0f}) {
            pulse::ui::math::FormulaLayout box, base;
            const bool ordinary = baseline && pulse::ui::math::BuildMathLayout(factory.Get(),face.Get(),metrics,baseline.root,16*scale,false,base,scale);
            check(ordinary && syntax && pulse::ui::math::BuildMathLayout(factory.Get(),face.Get(),metrics,syntax.root,16*scale,false,box,scale) && std::abs(box.width-base.width-10*96/72.27f*scale)<0.05f,"explicit pt spacing follows production device scale");
        }
    }
    const auto file=std::filesystem::current_path()/L"bench_data"/(L"xml-attribute-audit-"+std::to_wstring(GetCurrentProcessId())+L".xml");
    {std::ofstream out(file); out << "<root title=\"A&amp;B &quot;quoted&quot; &lt;x&gt;\" tab=\"&#9;\"><child/></root>";}
    std::wstring tree_payload; uint32_t bytes=0;bool cut=false;pulse::ipc::PreviewTextEncoding encoding{};
    pulse::ui::TreeView tree;
    bool parsed=pulse::preview::MakeTreeDocument(file.wstring(),L".xml",tree_payload,bytes,cut,encoding) && tree.SetPayload(tree_payload);
    tree.Reveal(0); const auto copied=tree.CurrentValue();
    ComPtr<IStream> stream;
    ComPtr<IXmlReader> reader;
    bool semantic = SUCCEEDED(CreateStreamOnHGlobal(nullptr,TRUE,&stream));
    if (semantic) {
        const wchar_t bom = 0xfeff;
        semantic = SUCCEEDED(stream->Write(&bom,sizeof(bom),nullptr)) && SUCCEEDED(stream->Write(copied.data(),static_cast<ULONG>(copied.size()*sizeof(wchar_t)),nullptr));
        LARGE_INTEGER start{};stream->Seek(start,STREAM_SEEK_SET,nullptr);
        semantic = semantic && SUCCEEDED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(reader.GetAddressOf()),nullptr)) && SUCCEEDED(reader->SetInput(stream.Get()));
    }
    XmlNodeType type{};
    if (semantic) {
        while (reader->Read(&type)==S_OK && type!=XmlNodeType_Element) {}
        const wchar_t* value = nullptr;UINT length = 0;
        semantic = type==XmlNodeType_Element && reader->MoveToAttributeByName(L"title",nullptr)==S_OK && reader->GetValue(&value,&length)==S_OK && std::wstring_view(value,length)==L"A&B \"quoted\" <x>";
        reader->MoveToElement();
        semantic = semantic && reader->MoveToAttributeByName(L"tab",nullptr)==S_OK && reader->GetValue(&value,&length)==S_OK && std::wstring_view(value,length)==L"\t";
    }
    check(parsed && semantic,"independent XmlLite parses copied subtree with equivalent escaped attribute values");
    std::error_code ec;std::filesystem::remove(file,ec);
    dc->SetTarget(nullptr);
    CoUninitialize();
    return failures ? 1 : 0;
}
