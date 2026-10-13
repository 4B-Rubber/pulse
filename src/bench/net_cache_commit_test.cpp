#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <barrier>
#include <atomic>
#include <cstdio>
#include <io.h>

static std::atomic<bool> pause_open{false};
static HANDLE open_entered = nullptr, open_release = nullptr;
static FILE* CacheTestOpen(int descriptor, const char* mode) {
    FILE* stream = _fdopen(descriptor, mode);
    if (stream && pause_open.exchange(false)) {
        SetEvent(open_entered);
        WaitForSingleObject(open_release, 5000);
    }
    return stream;
}

static std::wstring profile;
static HRESULT WINAPI CacheTestProfile(HWND, int, HANDLE, DWORD, LPWSTR output) {
    return wcscpy_s(output, MAX_PATH, profile.c_str()) == 0 ? S_OK : E_FAIL;
}
#define SHGetFolderPathW CacheTestProfile
#define _fdopen CacheTestOpen
#include "../fs/fs_net_cache.cpp"
#undef SHGetFolderPathW
#undef _fdopen

namespace {
namespace files = std::filesystem;
using namespace pulse::fs;
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!ok) ++failures;
}
using Ticket = NetSnapshotWrite;
Ticket Begin(const std::wstring& path) { return BeginNetSnapshotWrite(path); }
SnapshotPtr Snapshot(const std::wstring& name, size_t count = 1) {
    auto result = std::make_shared<std::vector<DirEntry>>(count);
    for (size_t i = 0; i < count; ++i) {
        (*result)[i].name = name + std::to_wstring(i);
        (*result)[i].size = i;
        (*result)[i].attrs = FILE_ATTRIBUTE_NORMAL;
    }
    return result;
}
bool Matches(const std::wstring& path, const SnapshotPtr& expected) {
    const auto actual = LoadNetSnapshot(path);
    if (!actual || actual->size() != expected->size()) return false;
    for (size_t i = 0; i < actual->size(); ++i)
        if ((*actual)[i].name != (*expected)[i].name || (*actual)[i].size != (*expected)[i].size)
            return false;
    return true;
}
size_t Temps(const files::path& root) {
    size_t count = 0;
    for (const auto& file : files::recursive_directory_iterator(root))
        if (file.path().extension() == L".tmp") ++count;
    return count;
}
void Run(const files::path& root) {
    const std::wstring path = L"\\\\cache-fixture\\share\\folder";
    const auto old_data = Snapshot(L"old-"), new_data = Snapshot(L"new-");
    auto old = Begin(path);
    auto newer = Begin(path);
    Check(!files::exists(root / L"Pulse"), "request registration does no disk I/O");
    Check(SaveNetSnapshot(newer, new_data), "new request commits first");
    Check(!SaveNetSnapshot(old, old_data), "late old request is rejected");
    Check(Matches(path, new_data), "fresh disk reload retains the newer directory");

    auto equivalent_old = Begin(L"\\\\cache-fixture\\share\\one\\..\\folder");
    auto equivalent_new = Begin(path + L"\\");
    Check(SaveNetSnapshot(equivalent_new, new_data) && !SaveNetSnapshot(equivalent_old, old_data),
          "normalized aliases share request order");
    auto pending_old = Begin(path);
    auto pending_new = Begin(path);
    Check(!SaveNetSnapshot(pending_old, old_data) && Matches(path, new_data),
          "new request supersedes old before its own save or failure");
    Check(!SaveNetSnapshot(pending_new, nullptr) && Matches(path, new_data),
          "failed new enumeration preserves last good cache");

    const std::wstring other = L"\\\\cache-fixture\\share\\other";
    auto independent = Begin(other);
    auto newest = Begin(path);
    Check(SaveNetSnapshot(independent, old_data) && SaveNetSnapshot(newest, new_data),
          "different paths commit independently");
    Check(!SaveNetSnapshot(newest, old_data) && Matches(path, new_data),
          "one request cannot publish twice");

    std::vector<Ticket> tickets;
    for (int i = 0; i < 8; ++i) tickets.push_back(Begin(path));
    std::barrier start(8);
    std::atomic<int> committed{0};
    const auto large = Snapshot(L"complete-new-", 20000);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < tickets.size(); ++i) threads.emplace_back([&, i] {
        start.arrive_and_wait();
        if (SaveNetSnapshot(tickets[i], i == tickets.size() - 1 ? large : old_data)) ++committed;
    });
    for (auto& thread : threads) thread.join();
    Check(committed == 1, "concurrent requests publish only the latest generation");
    Check(Matches(path, large), "concurrent large snapshot is complete with no mixed entries");
    Check(Temps(root) == 0, "successful and stale writers leave no temporary files");

    auto delayed = Begin(path);
    open_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    open_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!open_entered || !open_release) throw std::runtime_error("barrier creation failed");
    pause_open = true;
    bool delayed_saved = true;
    std::thread delayed_writer([&] { delayed_saved = SaveNetSnapshot(delayed, old_data); });
    Check(WaitForSingleObject(open_entered, 3000) == WAIT_OBJECT_0, "old writer pauses with its exclusive temporary file open");
    const auto while_open = Begin(path);
    Check(SaveNetSnapshot(while_open, large), "new request commits while old temporary file is still open");
    Check(Matches(path, large), "new cache reloads before old writer is released");
    SetEvent(open_release);
    delayed_writer.join();
    CloseHandle(open_entered); CloseHandle(open_release);
    Check(!delayed_saved && Matches(path, large), "pre-publication check rejects writer superseded during serialization");
    Check(Temps(root) == 0, "stale in-flight writer removes only its own temporary file");

    auto shared_ticket = Begin(path);
    std::barrier same_start(4);
    committed = 0;
    threads.clear();
    for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
        same_start.arrive_and_wait();
        if (SaveNetSnapshot(shared_ticket, large)) ++committed;
    });
    for (auto& thread : threads) thread.join();
    Check(committed == 1 && Matches(path, large) && Temps(root) == 0,
          "duplicate concurrent publication is at most once with complete bytes and cleanup");

    const std::wstring failure_path = L"\\\\cache-fixture\\share\\fail";
    const files::path destination = CacheFile(failure_path);
    files::create_directory(destination);
    const files::path foreign_tmp = destination.wstring() + L".tmp";
    { std::ofstream sentinel(foreign_tmp); sentinel << "another writer owns this"; }
    Check(!SaveNetSnapshot(Begin(failure_path), old_data), "failed atomic replacement reports failure");
    std::ifstream sentinel(foreign_tmp);
    const std::string bytes(std::istreambuf_iterator<char>(sentinel), {});
    Check(bytes == "another writer owns this", "failed writer preserves another task's temporary file");
    sentinel.close();
    Check(Temps(root) == 1, "failure cleans only its own temporary file");
    files::remove(destination);
    auto retry = Begin(failure_path);
    const files::path retry_destination = CacheFile(failure_path);
    files::create_directory(retry_destination);
    Check(!SaveNetSnapshot(retry, old_data), "replacement failure does not report success");
    files::remove(retry_destination);
    Check(SaveNetSnapshot(retry, old_data) && Matches(failure_path, old_data),
          "same still-current request can retry a failed publication");
    Check(!SaveNetSnapshot(Begin(L"C:\\local"), old_data), "non-UNC request is not persisted");
    Check(!SaveNetSnapshot(Begin(path + std::wstring(1, L'\0') + L"hidden"), old_data),
          "invalid normalized request is rejected");
}
}
int wmain() {
    std::error_code setup_error;
    const auto parent = files::absolute(files::path(L"bench_data"), setup_error);
    if (!setup_error) files::create_directories(parent, setup_error);
    if (setup_error) {
        std::cerr << "[FAIL] private fixture parent creation: " << setup_error.message() << std::endl;
        return 2;
    }
    const auto root = parent /
        (L"cache-commit-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!files::create_directory(root, setup_error)) {
        std::cerr << "[FAIL] exclusive fixture creation: " << setup_error.message() << std::endl;
        return 2;
    }
    profile = root.wstring();
    try { Run(root); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    std::error_code error;
    files::remove_all(root, error); // Only the directory this process created above.
    Check(!error && !files::exists(root), "owned fixture cleanup completes");
    std::cout << "Failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
