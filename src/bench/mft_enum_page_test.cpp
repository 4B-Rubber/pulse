#include "../index/index_mft_enum_page.h"

#include <iostream>
#include <vector>

namespace {

std::vector<BYTE> MakePage(uint64_t next_cursor, size_t count) {
    constexpr size_t record_size = 64;
    std::vector<BYTE> bytes(sizeof(USN) + count * record_size);
    std::memcpy(bytes.data(), &next_cursor, sizeof(next_cursor));
    for (size_t i = 0; i < count; ++i) {
        USN_RECORD_V2 record{};
        record.RecordLength = record_size;
        record.MajorVersion = 2;
        // Deliberately unlike the next-page cursor, to catch FRN-based pagination.
        record.FileReferenceNumber = 9000 + i;
        record.FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
        record.FileNameLength = sizeof(WCHAR);
        record.FileName[0] = L'x';
        std::memcpy(bytes.data() + sizeof(USN) + i * record_size, &record, record_size);
    }
    return bytes;
}

template<class T>
void SetField(std::vector<BYTE>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + sizeof(USN) + offset, &value, sizeof(value));
}

} // namespace

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failures;
    };
    pulse::index::MftEnumPage result{};
    const auto first = MakePage(100, 2);
    const auto second = MakePage(300, 1);
    uint64_t cursor = 0;
    uint64_t count = 0;
    bool valid = true;
    for (const auto& bytes : {first, second}) {
        if (!pulse::index::ParseMftEnumPage(bytes, cursor, result)) {
            valid = false;
            break;
        }
        count += result.records;
        cursor = result.next_cursor;
    }
    check(valid && count == 3 && cursor == 300, "two pages count records and use header cursors");
    check(pulse::index::ParseMftEnumPage(MakePage(500, 0), cursor, result) &&
          result.records == 0 && result.next_cursor == 500, "header-only page advances");

    auto reject = [&](const std::vector<BYTE>& bytes, uint64_t start, const char* name) {
        result = {999, 999};
        check(!pulse::index::ParseMftEnumPage(bytes, start, result) &&
              result.records == 0 && result.next_cursor == 0, name);
    };
    reject({}, 0, "zero-byte success is invalid");
    reject(std::vector<BYTE>(sizeof(USN) - 1), 0, "short cursor is invalid");
    reject(first, 100, "unchanged cursor is invalid");
    reject(first, 101, "backward cursor is invalid");
    auto bad = first;
    bad.resize(sizeof(USN) + 3);
    reject(bad, 0, "short record header is invalid");
    bad = first;
    bad.pop_back();
    reject(bad, 0, "partial last record rejects entire page");
    bad = first;
    bad.push_back(0);
    reject(bad, 0, "trailing byte is invalid");
    for (DWORD length : {DWORD{0}, DWORD{8}, DWORD{65}, DWORD{4096}}) {
        bad = first;
        SetField(bad, offsetof(USN_RECORD_V2, RecordLength), length);
        reject(bad, 0, "invalid record length");
    }
    bad = first;
    SetField(bad, offsetof(USN_RECORD_V2, MajorVersion), WORD{3});
    reject(bad, 0, "unsupported major version");
    bad = first;
    SetField(bad, offsetof(USN_RECORD_V2, MinorVersion), WORD{1});
    reject(bad, 0, "unsupported minor version");
    for (WORD offset : {WORD{0}, WORD{59}, WORD{61}, WORD{66}}) {
        bad = first;
        SetField(bad, offsetof(USN_RECORD_V2, FileNameOffset), offset);
        reject(bad, 0, "invalid filename offset");
    }
    for (WORD length : {WORD{1}, WORD{6}, WORD{65534}}) {
        bad = first;
        SetField(bad, offsetof(USN_RECORD_V2, FileNameLength), length);
        reject(bad, 0, "invalid filename length");
    }
    return failures == 0 ? 0 : 1;
}
