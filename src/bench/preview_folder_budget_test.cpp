#include <windows.h>
#include <shlwapi.h>
#include <map>
#include <vector>
#include <string>
#include <cstdio>
#include "../preview_host/preview_integrity.h"
#include "../preview_host/preview_file_utils.h"
namespace fixture {
struct Directory { std::vector<WIN32_FIND_DATAW> rows; DWORD open_error = 0, end_error = ERROR_NO_MORE_FILES, attrs = FILE_ATTRIBUTE_DIRECTORY; };
struct Cursor { Directory* dir; size_t index; };
std::map<std::wstring, Directory> dirs;
ULONGLONG now = 0, cost = 0;
size_t opens = 0, closes = 0;
HANDLE WINAPI First(LPCWSTR path, FINDEX_INFO_LEVELS, LPVOID out, FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    ++opens; now += cost;
    auto it = dirs.find(path);
    if (it == dirs.end()) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    auto& d = it->second;
    if (d.open_error || d.rows.empty()) { SetLastError(d.open_error ? d.open_error : ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    *static_cast<WIN32_FIND_DATAW*>(out) = d.rows[0];
    return new Cursor{&d, 0};
}
BOOL WINAPI Next(HANDLE handle, LPWIN32_FIND_DATAW out) {
    now += cost;
    auto& c = *static_cast<Cursor*>(handle);
    if (++c.index == c.dir->rows.size()) { SetLastError(c.dir->end_error); return FALSE; }
    *out = c.dir->rows[c.index]; return TRUE;
}
BOOL WINAPI Close(HANDLE handle) { ++closes; delete static_cast<Cursor*>(handle); SetLastError(ERROR_SUCCESS); return TRUE; }
ULONGLONG WINAPI Tick() { return now; }
DWORD WINAPI Attributes(LPCWSTR path) {
    const auto it = dirs.find(std::wstring(path) + L"\\*");
    return it == dirs.end() ? INVALID_FILE_ATTRIBUTES : it->second.attrs;
}
}
#define FindFirstFileExW fixture::First
#define FindNextFileW fixture::Next
#define FindClose fixture::Close
#define GetTickCount64 fixture::Tick
#define GetFileAttributesW fixture::Attributes
#include "../preview_host/folder_listing.cpp"
#undef FindFirstFileExW
#undef FindNextFileW
#undef FindClose
#undef GetTickCount64
#undef GetFileAttributesW
int main() {
    using namespace pulse::preview;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    const DWORD bits[] = {FILE_ATTRIBUTE_OFFLINE, FILE_ATTRIBUTE_RECALL_ON_OPEN, FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, FILE_ATTRIBUTE_PINNED};
    for (unsigned mask = 0; mask < 16; ++mask) {
        DWORD attrs = FILE_ATTRIBUTE_NORMAL;
        for (unsigned i = 0; i < 4; ++i) if (mask & (1u << i)) attrs |= bits[i];
        check(IsOfflinePlaceholder(attrs) == ((mask & 7) != 0), "offline/recall/pinned truth table");
    }
    const std::wstring root = L"\\\\?\\C:\\pulse-synthetic-enumeration";
    auto reset = [&] { fixture::dirs.clear(); fixture::now = fixture::cost = 0; fixture::opens = fixture::closes = 0; fixture::dirs[root + L"\\*"]; };
    auto row = [](std::wstring name, DWORD attrs = FILE_ATTRIBUTE_NORMAL) { WIN32_FIND_DATAW fd{}; wcscpy_s(fd.cFileName, name.c_str()); fd.dwFileAttributes = attrs; fd.nFileSizeLow = 7; return fd; };
    std::wstring payload;
    auto run = [&](uint32_t budget = 1000, bool final = true) { return MakeFolderListing(root, budget, final, payload); };
    auto status = [&] { DecodeResult r; r.text = payload; return DescribeIntegrity(r, true); };
    reset(); check(run() && status().state == IntegrityState::Complete && status().loaded == 0 && status().total == 0, "empty directory excludes summary from loaded count");
    for (DWORD error : {DWORD(ERROR_ACCESS_DENIED), DWORD(ERROR_PATH_NOT_FOUND)}) {
        reset(); fixture::dirs[root + L"\\*"].open_error = error;
        check(!run(), "root enumeration failure returns failure without redundant probe");
    }
    reset(); fixture::dirs[root + L"\\*"].rows = {row(L"child", FILE_ATTRIBUTE_DIRECTORY)};
    fixture::dirs[root + L"\\child\\*"].open_error = ERROR_ACCESS_DENIED;
    check(run() && status().reason == IntegrityReason::ReadFailure && status().total == 0, "child access denial is partial with unknown total");
    reset(); auto& d = fixture::dirs[root + L"\\*"]; d.rows = {row(L"one")}; d.end_error = ERROR_BAD_NET_RESP;
    check(run() && status().reason == IntegrityReason::ReadFailure && status().loaded == 1 && fixture::closes == 1, "FindNext error survives FindClose overwriting LastError");
    reset(); fixture::dirs[root + L"\\*"].rows = {row(L"one")}; fixture::dirs[root + L"\\*"].end_error = ERROR_SUCCESS;
    check(run() && status().reason == IntegrityReason::ReadFailure, "failed enumeration with zero LastError is still partial");
    reset(); fixture::dirs[root + L"\\*"].rows = {row(L"one"), row(L"two")};
    check(run() && status().state == IntegrityState::Complete && status().loaded == 2 && status().total == 2, "normal EOF reports exact entry count");
    for (bool final : {false, true}) {
        reset(); for (int i = 0; i < 100; ++i) fixture::dirs[root + L"\\*"].rows.push_back(row(std::to_wstring(i)));
        fixture::cost = 10;
        check(run(35, final) && fixture::now <= 40 && status().state == IntegrityState::Partial && status().total == 0 &&
            payload.starts_with(final ? L"PULSEARC\t1\tDIR\t-\t1" : L"PULSEARC\t1\tDIR\t-\t2"), "flat enumeration observes budget and first/final pass state");
    }
    reset(); fixture::dirs[root + L"\\*"].rows = {row(L"one")}; fixture::cost = 100;
    check(run(35, false) && status().loaded == 0 && status().state == IntegrityState::Partial, "root open time is part of budget");
    for (bool long_names : {false, true}) {
        reset(); for (int i = 0; i < 4100; ++i) fixture::dirs[root + L"\\*"].rows.push_back(row((long_names ? std::wstring(240, L'x') : L"file") + std::to_wstring(i)));
        check(run() && status().reason == IntegrityReason::Limit && status().total == 4100 && status().loaded <= 4000 && status().loaded < status().total && payload.size() < pulse::ipc::kPreviewMaxArchiveChars, "display clipping keeps accurate scanned total and bounded payload");
    }
    reset(); fixture::dirs[root + L"\\*"].rows = {row(L"junction", FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT), row(L"cloud", FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE)};
    check(run() && fixture::opens == 1 && status().loaded == 2 && status().total == 0 && status().state == IntegrityState::Partial, "junction and offline folder are listed without traversal or a fabricated subtree total");
    reset(); fixture::dirs[root + L"\\*"].rows = {row(L"child", FILE_ATTRIBUTE_DIRECTORY)};
    for (int i = 0; i < 4100; ++i) fixture::dirs[root + L"\\child\\*"].rows.push_back(row(std::to_wstring(i)));
    check(run() && status().total == 4101 && status().loaded == 1 && status().reason == IntegrityReason::Limit,
        "whole-level display omission keeps full subtree statistics");
    for (DWORD empty_error : {DWORD(ERROR_FILE_NOT_FOUND), DWORD(ERROR_NO_MORE_FILES)}) {
        reset(); fixture::dirs[root + L"\\*"].open_error = empty_error;
        check(run() && status().state == IntegrityState::Complete, "empty EOF requires existing directory metadata");
        for (DWORD attrs : {DWORD(INVALID_FILE_ATTRIBUTES), DWORD(FILE_ATTRIBUTE_NORMAL)}) {
            fixture::dirs[root + L"\\*"].attrs = attrs;
            check(!run(), "disappeared or replaced root is not empty success");
        }
        reset(); fixture::dirs[root + L"\\*"].rows = {row(L"child", FILE_ATTRIBUTE_DIRECTORY)};
        auto& child = fixture::dirs[root + L"\\child\\*"];
        child.open_error = empty_error; child.attrs = INVALID_FILE_ATTRIBUTES;
        check(run() && status().reason == IntegrityReason::ReadFailure && status().total == 0,
            "disappeared child leaves unknown total and read failure");
    }
    reset(); std::wstring nested = root;
    for (int depth = 0; depth < 66; ++depth) {
        fixture::dirs[nested + L"\\*"].rows = {row(L"child", FILE_ATTRIBUTE_DIRECTORY)};
        nested += L"\\child";
    }
    check(run() && status().state == IntegrityState::Partial && status().total == 0 &&
        payload.find(L"\tscan-limit\t0\n") != std::wstring::npos, "depth cap never advertises complete subtree total");
    reset(); fixture::dirs[root + L"\\*"].rows = {row(L"child", FILE_ATTRIBUTE_DIRECTORY)};
    fixture::dirs[root + L"\\child\\*"].rows = {row(L"file")}; fixture::cost = 10;
    check(run(25, false) && status().state == IntegrityState::Partial && status().total == 0 &&
        payload.find(L"\tscan-limit\t0\n") != std::wstring::npos, "deadline during child scan keeps subtree total unknown");
    return failures ? 1 : 0;
}
