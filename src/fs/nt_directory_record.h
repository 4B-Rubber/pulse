#pragma once
#include <windows.h>
#include <cstddef>
#include <stdexcept>

namespace pulse::fs {
struct NtFileFullDirInformation {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    WCHAR FileName[1];
};
static_assert(offsetof(NtFileFullDirInformation, FileName) == 68);
inline void ValidateNtDirectoryRecord(const NtFileFullDirInformation* record, size_t available) {
    constexpr size_t header = offsetof(NtFileFullDirInformation, FileName);
    if (available < header || record->FileNameLength % sizeof(WCHAR) || record->FileNameLength > available - header ||
        (record->NextEntryOffset && (record->NextEntryOffset < header + record->FileNameLength || record->NextEntryOffset > available - header)))
        throw std::runtime_error("Invalid NT directory record");
}
}
