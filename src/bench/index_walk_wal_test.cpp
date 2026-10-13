#include "../index/index_engine.h"
#include "../index/index_paths.h"
#include "../index/index_delta.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
namespace pulse::index {
struct EngineTestAccess {
    static bool Load(Engine& engine) { return engine.TryLoadCache(); }
    static void Add(Engine& engine, const std::wstring& root, const std::wstring& relative, DWORD action = FILE_ACTION_ADDED) {
        const auto bytes = offsetof(FILE_NOTIFY_INFORMATION, FileName) + relative.size() * sizeof(wchar_t);
        std::vector<BYTE> buffer(bytes);
        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());
        info->Action = action;
        info->FileNameLength = static_cast<DWORD>(relative.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, relative.data(), info->FileNameLength);
        engine.ApplyNotifyLocked(root, buffer.data(), static_cast<DWORD>(bytes));
    }
    static uint64_t Epoch(const Engine& engine) { return engine.filter_epoch_; }
    static bool Pending(Engine& engine) { return engine.DeltaFor(0) && engine.DeltaFor(0)->HasPending(); }
    static void Flush(Engine& engine) { engine.FlushDeltas(); }
};
}
int main() {
    using namespace pulse::index;
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(stdout); failures += !ok; };
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"walk_wal_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    if (!fs::create_directory(root)) return 2;
    const auto files = root / L"files", index = root / L"index";
    fs::create_directory(files); fs::create_directory(index);
    SetMachineIndexScope(false); SetActiveIndexDirectory(index.wstring());
    auto write = [&](const wchar_t* name, const char* text) { std::ofstream out(files / name, std::ios::binary | std::ios::trunc); out << text; };
    auto result = [&](Engine& engine, const wchar_t* name) { Query q; q.needle = L"\"" + std::wstring(name) + L"\""; q.path_prefix = files.wstring(); return engine.Search(q); };
    auto wait = [&](auto predicate) { const auto deadline = GetTickCount64() + 15000; while (GetTickCount64() < deadline) { if (predicate()) return true; Sleep(20); } return false; };
    write(L"original.txt", "a");
    {
        Engine engine; engine.StartFixture(nullptr, 0, files.wstring());
        const bool ready = wait([&] { return engine.Ready() && result(engine,L"original.txt").total == 1; });
        check(ready, "walk base published from isolated fixture");
        if (ready) {
            write(L"added.txt", "abc");
            check(wait([&] { return result(engine,L"added.txt").total == 1; }), "real create notification applied");
            fs::rename(files / L"original.txt", files / L"renamed.txt");
            check(wait([&] { return result(engine,L"renamed.txt").total == 1 && result(engine,L"original.txt").total == 0; }), "real rename notification applied");
            fs::remove(files / L"added.txt");
            check(wait([&] { return result(engine,L"added.txt").total == 0; }), "real delete notification applied");
            write(L"renamed.txt", "1234567");
            check(wait([&] { auto r = result(engine,L"renamed.txt"); return r.hits.size() == 1 && r.hits[0].size == 7; }), "real metadata notification applied");
        }
        engine.Stop();
    }
    for (int restart = 0; restart < 2; ++restart) {
        Engine replay;
        check(EngineTestAccess::Load(replay), "base and production walk WAL replay without a filesystem scan");
        auto r = result(replay,L"renamed.txt");
        check(r.hits.size() == 1 && r.hits[0].size == 7 && result(replay,L"original.txt").total == 0 && result(replay,L"added.txt").total == 0,
            "create rename delete and metadata survive repeated replay");
    }
    {
        Engine writer;
        check(EngineTestAccess::Load(writer), "load base for controlled WAL write failure");
        fs::create_directories(files / L"nested" / L"deep");
        write(L"nested\\deep\\retry.txt", "retry");
        EngineTestAccess::Add(writer, files.wstring(), L"nested\\deep\\retry.txt");
        check(EngineTestAccess::Pending(writer), "production add queues missing ancestors and file before flush");
        HANDLE lock = CreateFileW(DeltaFilePath(0).c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED range{};
        const bool locked = lock != INVALID_HANDLE_VALUE && LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, 0, &range);
        check(locked, "owned WAL byte range locked to inject a write failure");
        if (locked) {
            EngineTestAccess::Flush(writer);
            check(EngineTestAccess::Pending(writer), "failed production flush retains pending notification records");
            UnlockFileEx(lock, 0, MAXDWORD, 0, &range);
        }
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        EngineTestAccess::Flush(writer);
        check(!EngineTestAccess::Pending(writer), "flush retries successfully after write access recovers");
    }
    {
        Engine replay;
        check(EngineTestAccess::Load(replay) && result(replay, L"retry.txt").total == 1,
            "retried WAL restores implicit parents and their child");
    }
    {
        Engine writer;
        check(EngineTestAccess::Load(writer), "attribute-cache fixture loaded");
        auto filtered = [&](const wchar_t* text) { Query q; q.needle = text; q.path_prefix = files.wstring(); return writer.Search(q).total; };
        check(filtered(L"size:>100") == 0, "size cache starts with no matches");
        write(L"renamed.txt", std::string(150, 'x').c_str());
        EngineTestAccess::Add(writer, files.wstring(), L"renamed.txt", FILE_ACTION_MODIFIED);
        check(filtered(L"size:>100") == 1, "metadata-only growth invalidates empty size cache");
        write(L"renamed.txt", "123");
        EngineTestAccess::Add(writer, files.wstring(), L"renamed.txt", FILE_ACTION_MODIFIED);
        check(filtered(L"size:>100") == 0, "metadata-only shrink removes old size match");
        auto set_year = [&](WORD year) {
            SYSTEMTIME time{}; time.wYear = year; time.wMonth = 1; time.wDay = 2;
            FILETIME stamp{}; SystemTimeToFileTime(&time, &stamp);
            HANDLE file = CreateFileW((files / L"renamed.txt").c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            const bool ok = file != INVALID_HANDLE_VALUE && SetFileTime(file, nullptr, nullptr, &stamp);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            check(ok, "owned file timestamp changed without a structural notification");
            EngineTestAccess::Add(writer, files.wstring(), L"renamed.txt", FILE_ACTION_MODIFIED);
        };
        set_year(2020);
        check(filtered(L"dm:2020") == 1, "date cache starts with one match");
        set_year(2030);
        check(filtered(L"dm:2020") == 0, "mtime-only change invalidates old date match");
        const auto epoch = EngineTestAccess::Epoch(writer);
        EngineTestAccess::Add(writer, files.wstring(), L"renamed.txt", FILE_ACTION_MODIFIED);
        check(EngineTestAccess::Epoch(writer) == epoch, "unchanged metadata notification preserves filter epoch");
    }
    fs::rename(files / L"renamed.txt", files / L"offline.txt");
    write(L"offline_added.txt", "offline");
    {
        Engine engine; engine.StartFixture(nullptr, 0, files.wstring());
        check(wait([&] { return result(engine,L"offline.txt").total == 1 && result(engine,L"offline_added.txt").total == 1 && result(engine,L"renamed.txt").total == 0; }),
            "restart reconciles changes made while helper was stopped");
        engine.Stop();
    }
    {
        Engine replay;
        check(EngineTestAccess::Load(replay), "reconciled base loads after another stop");
        check(result(replay,L"offline.txt").total == 1 && result(replay,L"offline_added.txt").total == 1 && result(replay,L"renamed.txt").total == 0,
            "offline reconciliation remains durable on second restart");
    }
    SetActiveIndexDirectory(L"");
    std::error_code error; fs::remove_all(root, error);
    check(!error && !fs::exists(root), "owned fixture and index files cleaned up");
    return failures ? 1 : 0;
}
