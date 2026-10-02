// Pulse.Preview.exe: serves preview requests from Pulse over a named pipe.
// Decoding lives in preview_decoders.cpp, details-pane properties in
// preview_properties.cpp; this file only runs the request loop.

#include "../ipc/preview_protocol.h"
#include "../common/current_user_security.h"
#include "../common/crash_reporter.h"
#include "preview_decoders.h"
#include "preview_file_utils.h"
#include "preview_properties.h"
#include <windows.h>
#include <objbase.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pulse;
using preview::ExtensionOf;
using preview::IsOfflinePlaceholder;
using preview::PreviewPropertyValue;
using preview::ReadProperties;

namespace {

bool WriteString(HANDLE pipe, const std::wstring& value) {
    const uint32_t chars = static_cast<uint32_t>(value.size());
    return ipc::WriteAll(pipe, &chars, sizeof(chars)) &&
        (chars == 0 || ipc::WriteAll(pipe, value.data(), chars * sizeof(wchar_t)));
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    crash::Initialize({crash::ProcessRole::Preview, false, {}});
    if (__argc < 2) return 2;
    const DWORD owner = wcstoul(__wargv[1], nullptr, 10);
    const std::wstring pipeName = ipc::PreviewPipeName(owner);
    CurrentUserSecurityAttributes pipeSecurity;
    if (!pipeSecurity) return 3;
    HANDLE pipe = CreateNamedPipeW(pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64*1024, 64*1024, 0, pipeSecurity.get());
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError()!=ERROR_PIPE_CONNECTED) { CloseHandle(pipe); return 4; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        ipc::PreviewRequest req;
        if (!ipc::ReadAll(pipe, &req, sizeof(req)) || req.magic!=ipc::kPreviewMagic ||
            req.path_chars==0 || req.path_chars>32768) break;
        std::wstring path(req.path_chars, L'\0');
        if (!ipc::ReadAll(pipe, path.data(), req.path_chars*sizeof(wchar_t))) break;
        std::vector<uint8_t> pixels;
        std::wstring previewText;
        std::wstring errorText;
        std::vector<PreviewPropertyValue> properties;
        UINT w=0,h=0,stride=0;
        UINT source_w=0, source_h=0;
        ipc::PreviewResponse response{};
        response.request_id=req.request_id;
        response.generation=req.generation;

        bool made = false;
        if (req.kind == ipc::PreviewRequestKind::Properties) {
            if (!IsOfflinePlaceholder(req.attrs)) properties = ReadProperties(path);
            response.property_count = static_cast<uint32_t>(properties.size());
            response.status = 0;
            made = true;
        } else {
            const std::wstring extension = ExtensionOf(path);
            const UINT cap = ipc::ClampPreviewPixelSize(req.pixel_size, false);
            const preview::DecodeRequest decode{req, path, extension,
                (req.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0, IsOfflinePlaceholder(req.attrs),
                cap, (req.flags & ipc::kPreviewRequestFlagGrid) != 0 ||
                     cap <= preview::kGridThumbnailEdge};
            preview::DecodeResult result;
            made = preview::DecodeContent(decode, result);
            pixels = std::move(result.pixels);
            w = result.width; h = result.height; stride = result.stride;
            source_w = result.source_width; source_h = result.source_height;
            previewText = std::move(result.text);
            errorText = std::move(result.error);
            response.kind = result.kind;
            response.frame_count = result.frame_count;
            response.frame_delay_ms = result.frame_delay_ms;
            response.loop_count = result.loop_count;
            const bool truncated = result.truncated;
            const uint32_t bytesRead = result.bytes_read;
            response.status = made ? 0 : 1;
            if (!made) {
                response.kind = ipc::PreviewContentKind::Unsupported;
                if (IsOfflinePlaceholder(req.attrs)) errorText = L"offline-placeholder";
                else if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                    errorText = L"path-unavailable";
            }
            response.text_chars = static_cast<uint32_t>(previewText.size());
            response.error_chars = static_cast<uint32_t>(errorText.size());
            response.bytes_read = bytesRead;
            if (truncated) response.flags |= ipc::kPreviewFlagTruncated;
        }
        response.width=w; response.height=h; response.stride=stride;
        response.source_width = source_w;
        response.source_height = source_h;
        HANDLE mapping=nullptr; void* view=nullptr; std::wstring mappingName;
        if (made && response.kind == ipc::PreviewContentKind::Bitmap) {
            mappingName=L"Local\\PulsePreviewMap-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                        std::to_wstring(req.request_id)+L"-"+std::to_wstring(GetTickCount64());
            mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
                static_cast<DWORD>(pixels.size()),mappingName.c_str());
            if (mapping) view=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,pixels.size());
            if (!view) { response.status=2; mappingName.clear(); }
            else memcpy(view,pixels.data(),pixels.size());
        }
        response.mapping_chars=static_cast<uint32_t>(mappingName.size());
        bool ok=ipc::WriteAll(pipe,&response,sizeof(response));
        if (ok && !mappingName.empty()) ok=ipc::WriteAll(pipe,mappingName.data(),
            response.mapping_chars*sizeof(wchar_t));
        if (ok && !previewText.empty())
            ok=ipc::WriteAll(pipe,previewText.data(),response.text_chars*sizeof(wchar_t));
        if (ok && !errorText.empty())
            ok=ipc::WriteAll(pipe,errorText.data(),response.error_chars*sizeof(wchar_t));
        for (const auto& property : properties) {
            if (!ok) break;
            ok = WriteString(pipe, property.label) && WriteString(pipe, property.value);
        }
        if (ok && mapping) { unsigned char ack=0; ok=ipc::ReadAll(pipe,&ack,1); }
        if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping);
        if (!ok) break;
    }
    CoUninitialize(); DisconnectNamedPipe(pipe); CloseHandle(pipe); return 0;
}