#include <windows.h>
#include <winioctl.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
int read_count = 0;
int fail_at = -1;
bool short_read = false;
bool multi_run = false;
std::vector<BYTE> record;
BOOL WINAPI MockControl(HANDLE, DWORD, LPVOID, DWORD, LPVOID out, DWORD, LPDWORD got, LPOVERLAPPED) {
    auto& vd = *static_cast<NTFS_VOLUME_DATA_BUFFER*>(out);
    vd = {};
    vd.BytesPerFileRecordSegment = vd.BytesPerCluster = vd.BytesPerSector = 512;
    vd.MftValidDataLength.QuadPart = 24 * 1024 * 1024;
    *got = sizeof(vd);
    return TRUE;
}
BOOL WINAPI MockSeek(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD) { return TRUE; }
BOOL WINAPI MockRead(HANDLE, LPVOID out, DWORD bytes, LPDWORD got, LPOVERLAPPED);
}
#define DeviceIoControl MockControl
#define SetFilePointerEx MockSeek
#define ReadFile MockRead
#include "../index/index_mft.cpp"
#undef DeviceIoControl
#undef SetFilePointerEx
#undef ReadFile

namespace {
using namespace pulse::index;
void Put16(std::vector<BYTE>& b, size_t off, uint16_t value) { std::memcpy(b.data() + off, &value, 2); }
std::vector<BYTE> MakeRecord(bool runs) {
    std::vector<BYTE> b(512);
    auto* h = reinterpret_cast<FileRecord*>(b.data());
    h->magic = 0x454c4946;
    h->usa_off = 48;
    h->usa_count = 2;
    h->attr_off = 56;
    h->flags = 1;
    h->seq = 1;
    h->bytes_used = 176;
    h->bytes_alloc = 512;
    Put16(b, 48, 0xaaaa);
    Put16(b, 510, 0xaaaa);
    auto* a = reinterpret_cast<AttrHeader*>(b.data() + 56);
    if (runs) {
        a->type = kAttrData;
        a->length = 88;
        a->non_resident = 1;
        auto* nr = reinterpret_cast<AttrNonResident*>(b.data() + 72);
        nr->pairs_off = 64;
        // Three separate 8 MiB runs (16384 clusters each).
        const BYTE pairs[] = {0x22, 0, 0x40, 1, 0, 0x22, 0, 0x40, 0, 0x40,
                              0x22, 0, 0x40, 0, 0x40, 0};
        std::memcpy(b.data() + 120, pairs, sizeof(pairs));
    } else {
        a->type = kAttrFileName;
        a->length = 96;
        auto* r = reinterpret_cast<AttrResident*>(b.data() + 72);
        r->value_off = 24;
        r->value_len = 68;
        b[80 + 64] = 1;
        b[80 + 65] = 1;
        Put16(b, 80 + 66, L'x');
    }
    return b;
}
BOOL WINAPI MockRead(HANDLE, LPVOID out, DWORD bytes, LPDWORD got, LPOVERLAPPED) {
    const int call = read_count++;
    if (call == fail_at) {
        *got = short_read ? bytes / 2 : 0;
        return short_read ? TRUE : FALSE;
    }
    auto* dst = static_cast<BYTE*>(out);
    if (call == 0 && multi_run) {
        auto first = MakeRecord(true);
        std::memcpy(dst, first.data(), bytes);
    } else {
        for (DWORD off = 0; off < bytes; off += 512)
            std::memcpy(dst + off, record.data(), 512);
    }
    *got = bytes;
    return TRUE;
}
int passed = 0, failed = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    ok ? ++passed : ++failed;
}
void RunCase(int failure, bool short_io, bool runs, int stop_after, bool callback_stop) {
    read_count = 0;
    fail_at = failure;
    short_read = short_io;
    multi_run = runs;
    std::atomic<bool> running{stop_after != 0};
    size_t count = 0;
    auto result = EnumerateMft(nullptr, &running, {}, [&](MftFile&& f) {
        ++count;
        if (f.name != L"x") ++failed;
        if (stop_after > 0 && count == static_cast<size_t>(stop_after)) {
            if (callback_stop) return false;
            running = false;
        }
        return true;
    });
    const auto expected = stop_after >= 0 ? MftReadResult::Stopped :
        failure >= 0 ? MftReadResult::Failed : MftReadResult::Complete;
    Check(result == expected, "complete / failed / stopped classification");
    if (failure > 1) Check(count == static_cast<size_t>(failure - 1) * 16384, "failure after valid partial records");
    if (expected == MftReadResult::Complete) Check(count == 49152, "complete reads every record");
    if (expected == MftReadResult::Failed) Check(read_count == failure + 1, "read failure terminates immediately");
}
}
int main() {
    record = MakeRecord(false);
    for (bool runs : {false, true}) {
        RunCase(-1, false, runs, -1, false);
        for (int failure = 0; failure <= 3; ++failure) {
            RunCase(failure, false, runs, -1, false);
            RunCase(failure, true, runs, -1, false);
        }
        RunCase(-1, false, runs, 0, false);
        RunCase(-1, false, runs, 1, false);
        RunCase(-1, false, runs, 1, true);
        RunCase(-1, false, runs, 49152, false);
    }
    std::printf("%d PASS / %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}
