#include "../fs/fs_net_cache.h"
#include "../app/unc_probe_scheduler.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <functional>

namespace {
using namespace pulse::fs;
std::filesystem::path directory;
int failures = 0;
void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
struct Gate {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL result = TRUE;
    ~Gate() { CloseHandle(entered); CloseHandle(release); }
};
std::mutex gates_mutex;
std::map<std::wstring, std::shared_ptr<Gate>> gates;
std::shared_ptr<Gate> commit_gate;
std::atomic<uint64_t> blocked_generation{0};
constexpr UINT kProbe = WM_APP + 7;
pulse::app::UncProbeScheduler scheduler;
std::map<std::wstring, NetStatus> status;
std::vector<UncProbeResult> received;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kProbe) {
        std::unique_ptr<UncProbeResult> result(reinterpret_cast<UncProbeResult*>(lp));
        scheduler.Finish(result->probe_id);
        if (scheduler.Accept(result->unc, result->probe_id, result->completed))
            status[result->unc] = result->status;
        received.push_back(*result);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
bool PumpUntil(const std::function<bool()>& condition) {
    const auto deadline = GetTickCount64() + 5000;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (condition()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    return false;
}
bool Received(uint64_t id, bool completed) {
    for (const auto& item : received) if (item.probe_id == id && item.completed == completed) return true;
    return false;
}
std::shared_ptr<Gate> Plan(const std::wstring& path, BOOL result, bool released = false) {
    auto gate = std::make_shared<Gate>(); gate->result = result;
    if (released) SetEvent(gate->release);
    std::lock_guard lock(gates_mutex); gates[path] = gate;
    return gate;
}
SnapshotPtr Snapshot(const std::wstring& prefix, int count = 2) {
    auto entries = std::make_shared<std::vector<DirEntry>>();
    for (int i = 0; i < count; ++i) {
        DirEntry e; e.name = prefix + std::to_wstring(i); e.size = 123456 + i;
        entries->push_back(e);
    }
    return entries;
}
bool CompleteSnapshot(const SnapshotPtr& snapshot, int count) {
    if (!snapshot || snapshot->size() != static_cast<size_t>(count)) return false;
    const auto prefix = snapshot->front().name.substr(0, snapshot->front().name.find(L':') + 1);
    for (int i = 0; i < count; ++i)
        if ((*snapshot)[i].name != prefix + std::to_wstring(i) || (*snapshot)[i].size != 123456u + i) return false;
    return true;
}
}
namespace pulse::fs {
std::wstring NetCacheDirectoryForTest() { return directory.wstring(); }
DWORD ProbeTimeoutForTest() { return 30; }
unsigned ActiveUncProbesForTest();
BOOL ProbeAttributesForTest(const std::wstring& path) {
    if (path.starts_with(L"local:")) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        return GetFileAttributesExW(path.substr(6).c_str(), GetFileExInfoStandard, &data);
    }
    std::shared_ptr<Gate> gate;
    { std::lock_guard lock(gates_mutex); gate = gates.at(path); }
    SetEvent(gate->entered);
    WaitForSingleObject(gate->release, INFINITE);
    return gate->result;
}
void BeforeNetSnapshotCommitForTest(uint64_t generation) {
    if (generation != blocked_generation.load()) return;
    SetEvent(commit_gate->entered);
    WaitForSingleObject(commit_gate->release, INFINITE);
}
}
int wmain() {
    using namespace pulse::fs;
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    directory = std::filesystem::path(temp) / (L"PulseNetAudit-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(directory);
    const std::wstring unc = L"\\\\audit-host\\share\\folder";
    auto a = BeginNetSnapshotWrite(unc);
    commit_gate = std::make_shared<Gate>(); blocked_generation = a.Generation();
    bool saved_a = true;
    std::thread old_writer([&] { saved_a = SaveNetSnapshot(unc, Snapshot(L"old:"), a); });
    Check(WaitForSingleObject(commit_gate->entered, 5000) == WAIT_OBJECT_0, "old writer pauses after real disk write before commit");
    auto b = BeginNetSnapshotWrite(unc);
    Check(SaveNetSnapshot(unc, Snapshot(L"new:"), b), "new request commits while old request is delayed");
    SetEvent(commit_gate->release); old_writer.join(); blocked_generation = 0;
    auto loaded = LoadNetSnapshot(unc);
    Check(!saved_a && loaded && loaded->front().name == L"new:0", "late old generation cannot replace newer on-disk snapshot");
    auto disk_file = std::filesystem::directory_iterator(directory)->path();
    const auto foreign = directory / L"unrelated.tmp";
    std::ofstream(foreign) << "keep";
    HANDLE locked = CreateFileW(disk_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    auto c = BeginNetSnapshotWrite(unc);
    Check(locked != INVALID_HANDLE_VALUE && !SaveNetSnapshot(unc, Snapshot(L"failed:"), c), "replacement failure is reported while target is held open");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    loaded = LoadNetSnapshot(unc);
    Check(loaded && loaded->front().name == L"new:0" && std::filesystem::exists(foreign), "failed writer preserves previous cache and another task's temporary file");
    size_t temporary = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) if (entry.path().extension() == L".tmp") ++temporary;
    Check(temporary == 1, "stale and failed writers clean up only their own temporary files");
    const auto concurrent = BeginNetSnapshotWrite(unc);
    std::vector<std::thread> writers;
    std::atomic<int> saved{0};
    for (int i = 0; i < 8; ++i) writers.emplace_back([&, i] {
        if (SaveNetSnapshot(unc, Snapshot(L"writer" + std::to_wstring(i) + L":", 4000), concurrent)) ++saved;
    });
    for (auto& thread : writers) thread.join();
    loaded = LoadNetSnapshot(unc);
    Check(saved > 0 && CompleteSnapshot(loaded, 4000), "overlapping large writes publish one complete snapshot, never mixed bytes");
    Check(!SaveNetSnapshot(unc, Snapshot(L"invalid:", 500001), concurrent), "writer enforces reader entry bound before replacing cache");

    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = WindowProc; wc.lpszClassName = L"PulseNetAudit";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    const std::wstring path = L"probe:ordered";
    auto gate_a = Plan(path, TRUE); const auto id_a = scheduler.Begin(path);
    Check(StartUncProbe(hwnd, kProbe, path, id_a), "start actual asynchronous probe wrapper");
    Check(PumpUntil([&] { return Received(id_a, false); }) && status[path] == NetStatus::Offline, "timeout publishes offline without reading worker result");
    auto gate_b = Plan(path, FALSE, true); const auto id_b = scheduler.Begin(path);
    Check(StartUncProbe(hwnd, kProbe, path, id_b) && PumpUntil([&] { return Received(id_b, true); }), "newer same-path observation completes first");
    SetEvent(gate_a->release);
    Check(PumpUntil([&] { return Received(id_a, true); }) && status[path] == NetStatus::Offline, "real Win32 message delivery ignores stale late online result");
    const std::wstring recovery = L"probe:recovery";
    auto recovery_gate = Plan(recovery, TRUE); const auto recovery_id = scheduler.Begin(recovery);
    Check(StartUncProbe(hwnd, kProbe, recovery, recovery_id) && PumpUntil([&] { return Received(recovery_id, false); }), "recovery probe first times out");
    SetEvent(recovery_gate->release);
    Check(PumpUntil([&] { return Received(recovery_id, true); }) && status[recovery] != NetStatus::Offline, "same request may repair timeout status when no newer request exists");
    Check(PumpUntil([] { return ActiveUncProbesForTest() == 0; }), "completed probe handles and worker permits are released");
    const auto cross_a = scheduler.Begin(L"cross:a"); scheduler.Finish(cross_a);
    const auto cross_b = scheduler.Begin(L"cross:b");
    Check(scheduler.Accept(L"cross:a", cross_a, true) && scheduler.IsActive(cross_b), "cross-path completion remains valid without releasing another slot");
    scheduler.Accept(L"cross:b", cross_b, true); scheduler.Finish(cross_b);
    const auto local = L"local:" + disk_file.wstring(); const auto local_id = scheduler.Begin(local);
    Check(StartUncProbe(hwnd, kProbe, local, local_id) && PumpUntil([&] { return Received(local_id, true); }) && status[local] != NetStatus::Offline, "production wrapper completes real Windows file-attribute I/O");
    PumpUntil([] { return ActiveUncProbesForTest() == 0; });
    std::vector<std::shared_ptr<Gate>> blocked;
    std::vector<uint64_t> blocked_ids;
    for (int i = 0; i < 8; ++i) {
        auto name = L"probe:capacity" + std::to_wstring(i); blocked.push_back(Plan(name, TRUE));
        blocked_ids.push_back(scheduler.Begin(name));
        Check(StartUncProbe(hwnd, kProbe, name, blocked_ids.back()), "bounded probe worker starts");
    }
    Plan(L"probe:overflow", TRUE, true);
    Check(!StartUncProbe(hwnd, kProbe, L"probe:overflow", scheduler.Begin(L"probe:overflow")), "blocked I/O cannot create unbounded probe threads");
    SetEvent(blocked.front()->release);
    Check(PumpUntil([&] { return Received(blocked_ids.front(), true); }), "one blocked probe publishes final completion");
    const auto resumed_id = scheduler.Begin(L"probe:overflow");
    Check(StartUncProbe(hwnd, kProbe, L"probe:overflow", resumed_id) &&
        PumpUntil([&] { return Received(resumed_id, true); }),
        "capacity is reusable when completion reaches UI, allowing queued work to resume");
    DestroyWindow(hwnd);
    for (const auto& gate : blocked) SetEvent(gate->release);
    Check(PumpUntil([] { return ActiveUncProbesForTest() == 0; }), "closing destination window still releases all background probe resources");
    std::filesystem::remove_all(directory);
    Check(!std::filesystem::exists(directory), "isolated disk fixtures are cleaned up");
    return failures ? 1 : 0;
}
