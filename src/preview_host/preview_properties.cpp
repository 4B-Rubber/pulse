#include "preview_properties.h"
#include "preview_file_utils.h"
#include "video_codec.h"
#include "../common/preview_extensions.h"
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <string>
#include <vector>

using namespace pulse;
using Microsoft::WRL::ComPtr;
using preview::PreviewPropertyValue;

namespace {

void AddProperty(IPropertyStore* store, REFPROPERTYKEY key, const wchar_t* label,
                 std::vector<PreviewPropertyValue>& out) {
    if (!store || out.size() >= 6) return;
    PROPVARIANT value{};
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt != VT_EMPTY && value.vt != VT_NULL) {
        PWSTR formatted = nullptr;
        if (SUCCEEDED(PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &formatted)) &&
            formatted && *formatted) {
            const std::wstring display = IsEqualPropertyKey(key, PKEY_Video_Compression)
                ? preview::VideoCodecDisplayName(formatted) : std::wstring(formatted);
            if (!display.empty()) out.push_back({ label, display });
        }
        CoTaskMemFree(formatted);
    }
    PropVariantClear(&value);
}

bool ReadUintProperty(IPropertyStore* store, REFPROPERTYKEY key, uint32_t& value) {
    if (!store) return false;
    PROPVARIANT pv{};
    PropVariantInit(&pv);
    bool ok = false;
    if (SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_UI4) { value = pv.uintVal; ok = true; }
        else if (pv.vt == VT_I4 && pv.lVal > 0) { value = static_cast<uint32_t>(pv.lVal); ok = true; }
    }
    PropVariantClear(&pv);
    return ok;
}

} // namespace

namespace pulse::preview {

std::vector<PreviewPropertyValue> ReadProperties(const std::wstring& path) {
    std::vector<PreviewPropertyValue> out;
    const std::wstring shell_path = ShellPath(path);
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(shell_path.c_str(), nullptr, GPS_BESTEFFORT,
                                                 IID_PPV_ARGS(&store)))) return out;
    const std::wstring extension = ExtensionOf(path);
    if (IsOneOf(extension, {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff", L".webp", L".heic"})) {
        AddProperty(store.Get(), PKEY_Image_Dimensions, L"尺寸", out);
        AddProperty(store.Get(), PKEY_Photo_DateTaken, L"拍摄时间", out);
        AddProperty(store.Get(), PKEY_Photo_CameraModel, L"相机", out);
    } else if (IsOneOf(extension, {L".mp4", L".mkv", L".mov", L".avi", L".webm", L".wmv", L".m4v"})) {
        AddProperty(store.Get(), PKEY_Media_Duration, L"时长", out);
        uint32_t width = 0, height = 0;
        if (out.size() < 6 && ReadUintProperty(store.Get(), PKEY_Video_FrameWidth, width) &&
            ReadUintProperty(store.Get(), PKEY_Video_FrameHeight, height) &&
            width > 0 && height > 0) {
            wchar_t dims[64];
            swprintf_s(dims, L"%u x %u", width, height);
            out.push_back({ L"分辨率", dims });
        }
        AddProperty(store.Get(), PKEY_Video_FrameRate, L"帧率", out);
        AddProperty(store.Get(), PKEY_Video_Compression, L"编码格式", out);
    } else if (IsOneOf(extension, {L".mp3", L".wav", L".flac", L".m4a", L".aac"})) {
        AddProperty(store.Get(), PKEY_Media_Duration, L"时长", out);
        AddProperty(store.Get(), PKEY_Music_Artist, L"艺术家", out);
        AddProperty(store.Get(), PKEY_Music_AlbumTitle, L"专辑", out);
    } else if (IsOneOf(extension, {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp"})) {
        AddProperty(store.Get(), PKEY_Title, L"标题", out);
        AddProperty(store.Get(), PKEY_Author, L"作者", out);
        AddProperty(store.Get(), PKEY_Document_PageCount, L"页数", out);
    }
    return out;
}

} // namespace pulse::preview
