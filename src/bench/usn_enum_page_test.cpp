#include "../index/usn_enum_page.h"
#include <vector>
#include <cstdio>

int main() {
    using pulse::index::ReadUsnEnumerationPage;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    auto page = [](uint64_t cursor, uint64_t file) {
        std::vector<BYTE> bytes(sizeof(uint64_t) + sizeof(USN_RECORD_V2));
        memcpy(bytes.data(), &cursor, sizeof(cursor));
        auto* record = reinterpret_cast<USN_RECORD_V2*>(bytes.data() + sizeof(uint64_t));
        record->RecordLength = sizeof(USN_RECORD_V2); record->MajorVersion = 2;
        record->FileReferenceNumber = file; record->FileNameOffset = offsetof(USN_RECORD_V2, FileName);
        record->FileNameLength = sizeof(wchar_t); record->FileName[0] = L'x';
        return bytes;
    };
    uint64_t cursor = 0; unsigned records = 0;
    auto first = page(100, 7), second = page(200, 42);
    check(ReadUsnEnumerationPage(first.data(), first.size(), cursor, cursor, [&](const auto& record) {
        ++records; return record.FileReferenceNumber == 7; }), "first page uses leading cursor and emits correct record");
    check(cursor == 100 && ReadUsnEnumerationPage(second.data(), second.size(), cursor, cursor, [&](const auto& record) {
        ++records; return record.FileReferenceNumber == 42; }) && cursor == 200 && records == 2,
        "two-page cursor is independent of last record FRN");
    const auto reject = [&](std::vector<BYTE> bytes) {
        unsigned callbacks = 0; uint64_t next = 0;
        return !ReadUsnEnumerationPage(bytes.data(), bytes.size(), 0, next, [&](const auto&) { ++callbacks; return true; }) && callbacks == 0;
    };
    auto damaged = first; damaged.resize(sizeof(uint64_t) + 3);
    check(reject(damaged), "truncated header rejected before callback");
    damaged = first; reinterpret_cast<USN_RECORD_V2*>(damaged.data() + sizeof(uint64_t))->RecordLength = 0;
    check(reject(damaged), "zero record length rejected");
    damaged = first; reinterpret_cast<USN_RECORD_V2*>(damaged.data() + sizeof(uint64_t))->FileNameLength = 65534;
    check(reject(damaged), "filename outside record rejected");
    check(!ReadUsnEnumerationPage(second.data(), second.size(), cursor, cursor, [](const auto&) { return true; }),
        "nonadvancing cursor rejected");
    return failures ? 1 : 0;
}
