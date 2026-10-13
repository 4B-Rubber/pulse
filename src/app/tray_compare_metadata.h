#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
namespace pulse::app {
struct TrayMetadataKey {
    std::array<uint64_t, 2> ids{};
    std::array<std::wstring, 2> paths;
    bool operator==(const TrayMetadataKey&) const = default;
};
struct TrayMetadataSnapshot {
    enum class Status { Pending, Ready, Error } status = Status::Pending;
    uint64_t generation = 0;
    std::array<WIN32_FILE_ATTRIBUTE_DATA, 2> files{};
    DWORD error = ERROR_SUCCESS;
};
// One active query and one replacement pair. Destruction requests cancellation;
// the detached worker owns its state until a provider returns, never AppState.
class TrayCompareMetadata {
public:
    using Query = std::function<DWORD(const std::wstring&, WIN32_FILE_ATTRIBUTE_DATA&, const std::atomic<bool>&)>;
    explicit TrayCompareMetadata(Query query = {});
    ~TrayCompareMetadata();
    TrayMetadataSnapshot Read(const TrayMetadataKey& key, HWND notify);
    void Cancel();
    TrayCompareMetadata(const TrayCompareMetadata&) = delete;
    TrayCompareMetadata& operator=(const TrayCompareMetadata&) = delete;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
