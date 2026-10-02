#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace pulse::app {
enum class FolderSizeState { Manual, Calculating, Ready, Updating, Partial, Unavailable, Cached, Indexed };
struct FolderSizeValue {
    FolderSizeState state = FolderSizeState::Manual;
    uint64_t bytes = 0;
    bool has_value = false;
};
struct FolderSizeRequest {
    std::wstring path;
    bool automatic = true;
};

// Window-thread methods only exchange in-memory state. Enumeration, watches and
// cache I/O belong to the background worker, including network paths.
class FolderSizes {
public:
    FolderSizes();
    ~FolderSizes();
    FolderSizes(const FolderSizes&) = delete;
    FolderSizes& operator=(const FolderSizes&) = delete;
    void SetCachePath(std::function<std::wstring()> path);
    void SetIndexEnabled(bool enabled);
    void Sync(std::vector<FolderSizeRequest> visible, std::vector<std::wstring> watch_roots);
    void Calculate(const std::wstring& path);
    FolderSizeValue Get(const std::wstring& path) const;
    void Invalidate(const std::wstring& path);
    bool TakeChanged();
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
