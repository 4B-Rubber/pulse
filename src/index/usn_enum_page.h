#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace pulse::index {
// FSCTL_ENUM_USN_DATA begins with the next FRN cursor, not a USN_RECORD.
// Validate a whole page before consumers publish any of its records.
template<class Emit>
bool ReadUsnEnumerationPage(const BYTE* data, size_t bytes, uint64_t previous,
                            uint64_t& next, Emit&& emit) {
    if (!data || bytes < sizeof(uint64_t)) return false;
    uint64_t cursor = 0;
    memcpy(&cursor, data, sizeof(cursor));
    if (cursor <= previous) return false;
    constexpr size_t header = offsetof(USN_RECORD_V2, FileName);
    size_t offset = sizeof(uint64_t);
    while (offset < bytes) {
        if (bytes - offset < header) return false;
        const auto* record = reinterpret_cast<const USN_RECORD_V2*>(data + offset);
        if (record->MajorVersion != 2 || record->RecordLength < header ||
            record->RecordLength > bytes - offset || (record->RecordLength & 7) ||
            record->FileNameOffset < header || (record->FileNameOffset & 1) || (record->FileNameLength & 1) ||
            record->FileNameOffset > record->RecordLength ||
            record->FileNameLength > record->RecordLength - record->FileNameOffset) return false;
        offset += record->RecordLength;
    }
    for (offset = sizeof(uint64_t); offset < bytes;) {
        const auto* record = reinterpret_cast<const USN_RECORD_V2*>(data + offset);
        if (!emit(*record)) return false;
        offset += record->RecordLength;
    }
    next = cursor;
    return true;
}
}
