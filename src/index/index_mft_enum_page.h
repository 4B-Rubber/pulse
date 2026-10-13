#pragma once

#include <windows.h>
#include <winioctl.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace pulse::index {

struct MftEnumPage {
    uint64_t next_cursor = 0;
    uint64_t records = 0;
};

// Copy fixed fields: a malformed page need not contain aligned records.
inline bool ParseMftEnumPage(std::span<const BYTE> bytes, uint64_t cursor,
                             MftEnumPage& page) {
    page = {};
    if (bytes.size() < sizeof(USN)) return false;
    MftEnumPage parsed{};
    static_assert(sizeof(parsed.next_cursor) == sizeof(USN));
    std::memcpy(&parsed.next_cursor, bytes.data(), sizeof(USN));
    if (parsed.next_cursor <= cursor) return false;

    constexpr size_t header_size = offsetof(USN_RECORD_V2, FileName);
    size_t offset = sizeof(USN);
    while (offset < bytes.size()) {
        if (bytes.size() - offset < header_size) return false;
        USN_RECORD_V2 record{};
        std::memcpy(&record, bytes.data() + offset, header_size);
        if (record.MajorVersion != 2 || record.MinorVersion != 0 ||
            record.RecordLength < header_size ||
            record.RecordLength > bytes.size() - offset ||
            record.RecordLength % sizeof(uint64_t) != 0 ||
            record.FileNameOffset < header_size ||
            record.FileNameOffset % sizeof(WCHAR) != 0 ||
            record.FileNameLength % sizeof(WCHAR) != 0 ||
            record.FileNameOffset > record.RecordLength ||
            record.FileNameLength > record.RecordLength - record.FileNameOffset) {
            return false;
        }
        ++parsed.records;
        offset += record.RecordLength;
    }
    page = parsed;
    return true;
}

} // namespace pulse::index
