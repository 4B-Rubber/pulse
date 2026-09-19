#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

namespace pulse::index {
// Caller holds the Engine's exclusive lock. Retain every raw heap reference,
// including tombstones and nodes shadowed by replay patches: IDs can be reused.
// No mapped base names are visited. Validation/allocation precede any mutation.
template<class Nodes, class Patches>
DWORD CompactOverlayNamePool(std::vector<wchar_t>& pool, Nodes& nodes, Patches& patches) noexcept {
    auto visit = [&](auto&& fn) {
        for (auto& node : nodes) fn(node);
        for (auto& entry : patches) if (entry.second.has_name) fn(entry.second);
    };
    uint64_t chars = 0;
    bool valid = true;
    visit([&](const auto& ref) {
        const size_t off = ref.off;
        if (off > pool.size() || ref.len > pool.size() - off) valid = false;
        chars += ref.len;
    });
    if (!valid) return ERROR_INVALID_DATA;
    if (chars > UINT32_MAX) return ERROR_ARITHMETIC_OVERFLOW;
    // Unexpected aliasing must not turn a compaction into pool growth.
    if (chars > pool.size()) return ERROR_INVALID_DATA;
    try {
        std::vector<wchar_t> compact(static_cast<size_t>(chars));
        size_t offset = 0;
        visit([&](auto& ref) {
            if (ref.len) std::memcpy(compact.data() + offset, pool.data() + ref.off,
                static_cast<size_t>(ref.len) * sizeof(wchar_t));
            ref.off = static_cast<uint32_t>(offset);
            offset += ref.len;
        });
        pool.swap(compact);
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::length_error&) {
        return ERROR_ARITHMETIC_OVERFLOW;
    }
}
}
