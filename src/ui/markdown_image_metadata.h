#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pulse::ui {
struct MarkdownImageInfo {
    bool ready = false;
    std::wstring path;
    DWORD attrs = 0;
    uint64_t size = 0, modified = 0;
};

// Per-document results; a process-wide two-worker queue bounds slow devices.
class MarkdownImageMetadata {
public:
    using Resolver = std::function<MarkdownImageInfo(const std::wstring&, const std::wstring&)>;
    explicit MarkdownImageMetadata(Resolver resolver = {});
    ~MarkdownImageMetadata();
    MarkdownImageMetadata(const MarkdownImageMetadata&) = delete;
    MarkdownImageMetadata& operator=(const MarkdownImageMetadata&) = delete;
    void Reset();
    MarkdownImageInfo Lookup(const std::wstring& target, const std::wstring& base_dir, HWND notify);
private:
    struct State;
    std::shared_ptr<State> state_;
    Resolver resolver_;
};
}
