#include "../ui/bloom_accent_picker.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;
namespace {
int failures=0;
void Check(bool ok,const char* name) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",name); failures+=!ok; }
ULONG References(IUnknown* object) { object->AddRef(); return object->Release(); }
}
int main() {
    ComPtr<ID3D11Device> gpu;
    ComPtr<ID3D11DeviceContext> gpu_context;
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&gpu,nullptr,&gpu_context);
    Check(SUCCEEDED(hr),"private WARP device created without a user window");
    if(FAILED(hr)) return 2;
    ComPtr<IDXGIDevice> dxgi;
    hr=gpu.As(&dxgi);
    ComPtr<ID2D1Factory1> factory;
    if(SUCCEEDED(hr)) hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,IID_PPV_ARGS(&factory));
    ComPtr<ID2D1Device> device;
    if(SUCCEEDED(hr)) hr=factory->CreateDevice(dxgi.Get(),&device);
    ComPtr<ID2D1DeviceContext> dc;
    if(SUCCEEDED(hr)) hr=device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc);
    ComPtr<ID2D1Bitmap1> bitmap;
    const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
    if(SUCCEEDED(hr)) hr=dc->CreateBitmap(D2D1::SizeU(320,320),nullptr,0,&properties,&bitmap);
    Check(SUCCEEDED(hr),"private Direct2D draw target initialized");
    if(FAILED(hr)) return 2;
    dc->SetTarget(bitmap.Get());
    pulse::ui::BloomAccentPicker picker;
    picker.SetDisk(D2D1::RectF(10,10,310,310));
    pulse::ui::Theme theme{};
    auto frame=[&] {
        dc->BeginDraw(); dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
        picker.Draw(dc.Get(),theme);
        return SUCCEEDED(dc->EndDraw());
    };
    bool warm=true;
    for(int i=0;i<30;++i) warm=frame()&&warm;
    Check(warm,"production picker warmed with successful actual draw calls");
    const auto before=References(factory.Get());
    bool rendered=true;
    for(int i=0;i<160;++i) {
        picker.SetSelection(i%2==0,0x335577,true);
        picker.SetPointer(static_cast<float>(i%300),150,true);
        picker.SetPressed(i%pulse::ui::kBloomDotCount);
        picker.Tick(1.0f/60.0f);
        rendered=frame()&&rendered;
    }
    const auto after=References(factory.Get());
    Check(rendered,"ring solid hover and pressed states complete repeated frames");
    std::printf("[INFO] factory references before=%lu after=%lu\n",before,after);
    Check(before==after,"repeated production draws retain no extra Direct2D factory references");
    picker.Draw(nullptr,theme);
    Check(References(factory.Get())==after,"null draw context leaves factory ownership unchanged");
    dc->SetTarget(nullptr);
    return failures?1:0;
}
