#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>

namespace pulse::index {
enum class FilenameStage : size_t { Wait, Topology, Journal, Notify, DeltaFlush, Merge, Rebuild, Recovery, Count };

// Worker-owned counters; an idle worker writes once per minute. The fixed file
// name lives beside the index and is excluded by the index-artifact filter.
class FilenameTiming {
public:
    struct Token { uint64_t wall = 0, cpu = 0; };
    static Token Begin() noexcept;
    void End(FilenameStage stage, Token token, uint64_t changes = 0,
             DWORD error = ERROR_SUCCESS, const char* reason = "none", wchar_t volume = 0) noexcept;
    void Flush(bool force = false) noexcept;
private:
    struct Counter {
        uint64_t calls = 0, wall_us = 0, cpu_us = 0, changes = 0, errors = 0;
        DWORD last_error = 0;
        wchar_t volume = 0;
        const char* reason = "none";
    };
    std::array<Counter, static_cast<size_t>(FilenameStage::Count)> counters_{};
    uint64_t last_flush_ = 0;
};
}
