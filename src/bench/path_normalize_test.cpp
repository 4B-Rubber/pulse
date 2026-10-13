#include "../fs/fs_enum.h"
#include "../fs/fs_snapshot.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>

// Only redirect the cache's profile root, not any filesystem API. The real
// cache bytes are written below this test's atomically owned fixture.
#include <shlobj.h>
static std::wstring cache_fixture;
static HRESULT WINAPI NormalizeTestAppData(HWND, int, HANDLE, DWORD, LPWSTR path) {
    return wcscpy_s(path, MAX_PATH, cache_fixture.c_str()) == 0 ? S_OK : E_FAIL;
}
#define SHGetFolderPathW NormalizeTestAppData
#include "../fs/fs_net_cache.cpp"
#undef SHGetFolderPathW

namespace {
namespace files = std::filesystem;
int failures = 0;
void Check(bool ok, const char* message) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << message << '\n';
    if (!ok) ++failures;
}
std::wstring Extended(const std::wstring& path) {
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}
bool HasMarker(const std::wstring& path) {
    try {
        std::vector<pulse::fs::DirEntry> entries;
        pulse::fs::EnumerateDirectory(path, entries);
        return entries.size() == 1 && entries.front().name == L"owned-marker.txt";
    } catch (const std::exception&) { return false; }
}
void Run(const files::path& root) {
    using pulse::fs::NormalizePath;
    using pulse::fs::ParentPath;
    const std::wstring base = root.wstring();
    const std::wstring drive = base.substr(0, 2);
    const std::wstring target = base + L"\\target";
    const std::wstring canonical = Extended(target);
    Check(NormalizePath(L"target") == canonical, "M03-004 ordinary relative path uses this test process working directory");
    Check(NormalizePath(drive + L"target") == canonical, "M03-004 drive-relative path resolves before adding extended prefix");
    Check(NormalizePath(target.substr(2)) == canonical, "M03-004 root-relative path retains the current drive");
    Check(NormalizePath(drive) == Extended(drive + L"\\"), "M03-004 bare drive retains the existing drive-root product convention");
    Check(NormalizePath(base + L"\\one\\..\\target") == canonical, "M03-004 ordinary absolute parent segment is resolved");
    Check(NormalizePath(base + L"\\.\\target") == canonical, "M03-004 ordinary absolute current segment is resolved");
    Check(NormalizePath(L"one\\..\\target") == canonical, "M03-004 ordinary relative parent segment stays compatible");
    std::wstring forward = base + L"/one/../target";
    std::replace(forward.begin(), forward.end(), L'\\', L'/');
    Check(NormalizePath(forward) == canonical, "M03-004 forward separators are resolved before prefix conversion");
    Check(NormalizePath(target + L"\\") == canonical, "M03-004 non-root trailing separator is canonicalized");
    Check(NormalizePath(drive + L"\\") == Extended(drive + L"\\"), "M03-004 drive root keeps its separator");
    // UNC inputs below are strings only. Never enumerate or open these names.
    Check(NormalizePath(L"\\\\review-server\\share\\one\\..\\target") == L"\\\\?\\UNC\\review-server\\share\\target",
          "M03-004 ordinary UNC parent segment is resolved without network I/O");
    Check(NormalizePath(L"\\\\review-server\\share\\.\\target") == L"\\\\?\\UNC\\review-server\\share\\target",
          "M03-004 ordinary UNC current segment is resolved without network I/O");
    Check(NormalizePath(L"\\\\review-server") == L"\\\\?\\UNC\\review-server",
          "M03-004 server-only navigation representation remains supported");
    Check(NormalizePath(L"\\\\review-server\\share") == L"\\\\?\\UNC\\review-server\\share",
          "M03-004 ordinary UNC share representation remains supported");
    const std::wstring literal = Extended(base + L"\\literal. ");
    Check(NormalizePath(literal) == literal, "M03-004 explicit extended literal trailing dot and space are preserved");
    const std::wstring extended_dots = Extended(base + L"\\one\\..\\target");
    Check(NormalizePath(extended_dots) == extended_dots, "M03-004 explicit extended namespace is not reinterpreted as ordinary DOS input");
    const std::wstring extended_unc = L"\\\\?\\UNC\\review-server\\share\\one\\..\\target";
    Check(NormalizePath(extended_unc) == extended_unc, "M03-004 explicit extended UNC is preserved without network I/O");
    Check(NormalizePath(base + L"\\CaseSensitive") == Extended(base + L"\\CaseSensitive") &&
          NormalizePath(base + L"\\casesensitive") == Extended(base + L"\\casesensitive"),
          "M03-004 normalizer does not fold case-distinct names");
    Check(NormalizePath(L"pulse:recycle") == L"pulse:recycle" && NormalizePath(L"").empty(),
          "M03-004 virtual and intentional empty This-PC paths keep their contract");
    Check(ParentPath(base + L"\\one\\..\\target") == Extended(base),
          "M03-004 parent lookup shares ordinary path normalization");
    Check(ParentPath(drive + L"target") == Extended(base),
          "M03-004 parent lookup resolves drive-relative paths consistently");
    std::wstring long_path = base;
    for (int i = 0; i < 60; ++i) long_path += L"\\segment";
    Check(NormalizePath(long_path + L"\\..\\target") == Extended(long_path.substr(0, long_path.find_last_of(L'\\')) + L"\\target"),
          "M03-004 ordinary path beyond MAX_PATH still resolves dot segments");
    const std::wstring over_limit = base + L"\\" + std::wstring(40000, L'x');
    Check(NormalizePath(over_limit).empty(), "M03-004 over-limit resolution fails instead of manufacturing an extended path");
    // Exercise the actual in-memory cache; no application/profile cache I/O.
    pulse::fs::SnapshotStore store;
    auto snapshot = std::make_shared<std::vector<pulse::fs::DirEntry>>();
    snapshot->push_back({}); snapshot->front().name = L"owned-cache-entry";
    store.Update(target, 7, snapshot);
    Check(store.Peek(base + L"\\one\\..\\target") == snapshot,
          "M03-004 equivalent absolute-dot paths share one snapshot cache key");
    Check(store.Peek(drive + L"target") == snapshot,
          "M03-004 drive-relative lookup reaches the same snapshot cache key");
    uint64_t generation = 99;
    Check(store.GetOrStart(base + L"\\.\\target", generation) == snapshot && generation == 7,
          "M03-004 cache generation is shared across equivalent ordinary paths");
    store.MarkDirty(base + L"\\one\\..\\target");
    Check(store.IsDirty(target), "M03-004 dirty notifications use the same normalized cache key");
    store.Update(target, 8, snapshot);
    store.Update(L"", 5, snapshot);
    const auto entries_before = store.EntryCount();
    generation = 99;
    const auto invalid = store.GetOrStart(over_limit, generation);
    Check(!invalid && generation == 0 && store.EntryCount() == entries_before,
          "M03-004 invalid cache lookup neither aliases This-PC nor creates a bogus entry");
    store.Update(over_limit, 100, std::make_shared<std::vector<pulse::fs::DirEntry>>());
    store.Put(over_limit, std::make_shared<std::vector<pulse::fs::DirEntry>>());
    store.MarkDirty(over_limit);
    Check(store.Peek(L"") == snapshot && !store.IsDirty(L"") && store.EntryCount() == entries_before,
          "M03-004 invalid cache writes and dirty marks preserve the intentional empty key");
    Check(!store.Peek(over_limit) && !store.IsDirty(over_limit) && !store.Identity(over_limit).valid(),
          "M03-004 invalid cache queries return no snapshot identity or dirty state");
    Check(NormalizePath(target + std::wstring(1, L'\0') + L"hidden").empty(),
          "M03-004 an embedded NUL cannot silently shorten ordinary input");
    Check(NormalizePath(L"\\\\review-server\\share\\target\\") == L"\\\\?\\UNC\\review-server\\share\\target",
          "M03-004 ordinary UNC trailing separator shares the same cache key");
    pulse::fs::DirectoryIdentity direct_identity, relative_identity;
    Check(pulse::fs::QueryDirectoryIdentity(target, direct_identity) &&
          pulse::fs::QueryDirectoryIdentity(drive + L"one\\..\\target", relative_identity) &&
          pulse::fs::SameDirectoryIdentity(direct_identity, relative_identity),
          "M03-004 equivalent ordinary paths query the same actual owned directory identity");
    const std::wstring invalid_unc = L"\\\\review-server\\share\\" + std::wstring(40000, L'x');
    Check(pulse::fs::CacheFile(invalid_unc).empty() && !files::exists(root / L"Pulse"),
          "M03-004 invalid UNC cache input is rejected before creating any cache directory");
    const std::wstring unc = L"\\\\review-server\\share\\target";
    const auto cache_file = pulse::fs::CacheFile(unc);
    Check(pulse::fs::CacheFile(L"\\\\review-server\\share\\one\\..\\target") == cache_file,
          "M03-004 disk cache filename shares ordinary UNC normalization");
    Check(pulse::fs::SaveNetSnapshot(pulse::fs::BeginNetSnapshotWrite(unc), snapshot),
          "M03-004 real disk cache write remains inside the redirected owned profile");
    const auto cached = pulse::fs::LoadNetSnapshot(L"\\\\review-server\\share\\.\\target");
    Check(cached && cached->size() == 1 && cached->front().name == snapshot->front().name,
          "M03-004 equivalent UNC path loads the exact owned disk snapshot without network access");
    const auto cache_bytes = [&] {
        std::ifstream input(files::path(cache_file), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const std::string before_invalid = cache_bytes();
    Check(!pulse::fs::SaveNetSnapshot(pulse::fs::BeginNetSnapshotWrite(invalid_unc), snapshot) &&
          !pulse::fs::LoadNetSnapshot(invalid_unc) && cache_bytes() == before_invalid,
          "M03-004 invalid UNC cache read and write leave existing snapshot bytes unchanged");
    Check(HasMarker(drive + L"target"), "M03-004 real directory enumeration reaches the owned drive-relative target");
    Check(HasMarker(base + L"\\one\\..\\target"), "M03-004 real directory enumeration reaches the owned absolute-dot target");
}
}
int wmain() {
    std::cout << "[INFO] process-local CWD and privately created directories only; UNC cases are lexical only\n";
    const files::path previous = files::current_path();
    const files::path root = previous / L"bench_data" /
        (L"review-normalize-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    const bool created = files::create_directory(root, ec);
    Check(created && !ec, "M03-004 unique owned fixture created without reusing an existing directory");
    if (!created || ec) return 2;
    try {
        files::create_directory(root / L"one");
        files::create_directory(root / L"target");
        { std::ofstream out(root / L"target" / L"owned-marker.txt"); out << "owned-marker"; }
        files::current_path(root);
        cache_fixture = root.wstring();
        Run(root);
    } catch (const std::exception& error) {
        std::cout << "[FAIL] fixture exception: " << error.what() << '\n'; ++failures;
    }
    files::current_path(previous, ec);
    Check(!ec && files::current_path() == previous, "M03-004 test process restores its own previous working directory");
    files::remove_all(root, ec);
    Check(!ec && !files::exists(root), "M03-004 only the owned fixture is removed");
    return failures ? 1 : 0;
}
