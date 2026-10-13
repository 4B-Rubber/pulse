#include "shell_tag_menu.h"
#include "single_instance_coordinator.h"
#include "default_file_manager.h"
#include <objbase.h>

namespace pulse::app {
namespace {
constexpr ULONG_PTR kShellTagMessage = 0x50544147;
constexpr size_t kMaxRequestChars = 32768;
bool Field(const std::wstring& value) {
    return !value.empty() && value.find_first_of(L"\r\n") == std::wstring::npos &&
        value.find(L'\0') == std::wstring::npos;
}
}
ULONG_PTR ShellTagMessageId() { return kShellTagMessage; }
std::optional<ShellTagRequest> ParseShellTagArgs(int argc, wchar_t** argv) {
    for (int i = 1; i + 2 < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg != L"--tag" && arg != L"--tag-add" && arg != L"--tag-remove") continue;
        ShellTagRequest request{argv[i + 1], argv[i + 2]};
        request.action = arg == L"--tag-remove" ? ShellTagAction::Remove : ShellTagAction::Add;
        GUID id{};
        wchar_t text[40]{};
        if (!Field(request.tag_id) || !Field(request.path) || FAILED(CoCreateGuid(&id)) ||
            !StringFromGUID2(id, text, ARRAYSIZE(text))) return std::nullopt;
        request.operation_id = text;
        return request;
    }
    return std::nullopt;
}
std::wstring EncodeShellTagRequest(const ShellTagRequest& request) {
    if (!Field(request.tag_id) || !Field(request.path) || !Field(request.operation_id)) return {};
    auto payload = std::wstring(L"PULSE-TAG-2\n") +
        (request.action == ShellTagAction::Remove ? L"remove\n" : L"add\n") +
        request.operation_id + L"\n" + request.tag_id + L"\n" + request.path;
    return payload.size() < kMaxRequestChars ? payload : std::wstring{};
}
bool ForwardShellTagRequest(const ShellTagRequest& request, std::wstring_view endpoint_name) {
    auto normalized = request;
    if (!SingleInstanceCoordinator::NormalizeLaunchPath(request.path, normalized.path) ||
        normalized.path.empty() || normalized.path.starts_with(L"pulse:") ||
        IsThisPcArgument(normalized.path)) return false;
    const auto payload = EncodeShellTagRequest(normalized);
    if (payload.empty()) return false;
    COPYDATASTRUCT data{kShellTagMessage, static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t)),
        const_cast<wchar_t*>(payload.c_str())};
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        HWND hwnd = SingleInstanceCoordinator::FindPrimaryWindow(endpoint_name);
        if (hwnd) {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result)) return result != FALSE;
        }
        Sleep(25);
    }
    return false;
}
bool DecodeShellTagRequest(const COPYDATASTRUCT* data, ShellTagRequest& out) {
    if (!data || data->dwData != kShellTagMessage || !data->lpData ||
        data->cbData < 2 * sizeof(wchar_t) || data->cbData % sizeof(wchar_t) ||
        data->cbData > kMaxRequestChars * sizeof(wchar_t)) return false;
    const auto* text = static_cast<const wchar_t*>(data->lpData);
    const size_t count = data->cbData / sizeof(wchar_t);
    if (text[count - 1]) return false;
    const std::wstring payload(text, count - 1);
    if (payload.find(L'\0') != std::wstring::npos) return false;
    ShellTagRequest request;
    size_t begin = 0;
    if (payload.starts_with(L"PULSE-TAG-2\n")) {
        begin = 12;
        auto end = payload.find(L'\n', begin);
        const auto action = payload.substr(begin, end - begin);
        if (end == std::wstring::npos || (action != L"add" && action != L"remove")) return false;
        request.action = action == L"remove" ? ShellTagAction::Remove : ShellTagAction::Add;
        begin = end + 1; end = payload.find(L'\n', begin);
        if (end == std::wstring::npos) return false;
        request.operation_id = payload.substr(begin, end - begin);
        if (!Field(request.operation_id)) return false;
        begin = end + 1;
    }
    const auto split = payload.find(L'\n', begin);
    if (split == std::wstring::npos) return false;
    request.tag_id = payload.substr(begin, split - begin);
    request.path = payload.substr(split + 1);
    if (!Field(request.tag_id) || !Field(request.path)) return false;
    out = std::move(request);
    return true;
}
}
