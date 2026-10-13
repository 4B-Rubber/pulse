#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace pulse::app {

class SingleInstanceCoordinator {
public:
    enum class AcquireResult { Primary, Existing, Failed };

    SingleInstanceCoordinator() = default;
    ~SingleInstanceCoordinator();
    SingleInstanceCoordinator(const SingleInstanceCoordinator&) = delete;
    SingleInstanceCoordinator& operator=(const SingleInstanceCoordinator&) = delete;

    AcquireResult Acquire(std::wstring_view mutex_name = {});
    void Release();
    bool PublishEndpoint(HWND window);
    bool PublishWindow(HWND window) { return PublishEndpoint(window); }
    static std::optional<std::wstring> ResolveOpenPath(const std::wstring& path) {
        std::wstring normalized;
        if (!NormalizeLaunchPath(path, normalized)) return std::nullopt;
        return normalized;
    }
    bool OwnsEndpoint(HWND window) const;
    static HWND FindPrimaryWindow(std::wstring_view endpoint_name = {});
    static bool NormalizeLaunchPath(const std::wstring& input, std::wstring& output);
    bool ForwardOpenPath(const std::wstring& path, DWORD timeout_ms = 2000) const;
    enum class OpenAcceptance { Invalid, New, Duplicate };
    struct OpenRequest {
        std::array<unsigned char, 16> id{};
        uint64_t deadline = 0;
        std::wstring path;
    };
    static std::vector<unsigned char> EncodeOpenRequest(const OpenRequest& request);
    static bool DecodeOpenRequest(const COPYDATASTRUCT* data, OpenRequest& request);
    OpenAcceptance AcceptOpenRequest(const OpenRequest& request, uint64_t now);
    static ULONG_PTR OpenRequestMessageId() noexcept;

    static bool DecodeOpenPath(const COPYDATASTRUCT* data, std::wstring& path);
    static ULONG_PTR OpenPathMessageId() noexcept;
    static const wchar_t* WindowClassName() noexcept;

private:
    HANDLE mutex_ = nullptr;
    HANDLE endpoint_mapping_ = nullptr;
    void* endpoint_view_ = nullptr;
    HWND endpoint_window_ = nullptr;
    std::wstring endpoint_name_;
    std::wstring endpoint_nonce_;
    std::map<std::array<unsigned char, 16>, OpenRequest> accepted_;
};

} // namespace pulse::app
