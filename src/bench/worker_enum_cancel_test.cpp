#include "../app/app_worker.cpp"
#include <cstdio>
static HANDLE blocked, release_io, delivered;
static std::atomic<int> block_count{0}, callbacks{0};
namespace pulse::fs {
bool IsVirtualPath(const std::wstring& p) { return p.starts_with(L"pulse:"); }
bool IsUncPath(const std::wstring& p) { return p.starts_with(L"\\\\"); }
DWORD ReadReparseTag(const std::wstring&) { return 0; }
void EnumerateDirectory(const std::wstring&, std::vector<DirEntry>&, const std::atomic_bool*) {}
bool EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out, const EnumerateProgress& progress) {
    if (path.find(L"blocked") != std::wstring::npos) {
        ++block_count;
        SetEvent(blocked);
        WaitForSingleObject(release_io, INFINITE);
    }
    if (path.find(L"streaming") != std::wstring::npos) {
        // Two batches the way fs_enum reports them: the first one is published while
        // the scan still runs (after the pool's 120 ms throttle), the second is folded
        // into the final result. 2500 rows clear the 2048-row first batch; the growth
        // rule then holds the 3000-row batch back.
        for (int i = 0; i < 2500; ++i) {
            DirEntry entry;
            entry.name = L"row" + std::to_wstring(i);
            out.push_back(std::move(entry));
        }
        Sleep(150);
        if (progress) progress(out);
        for (int i = 2500; i < 3000; ++i) {
            DirEntry entry;
            entry.name = L"row" + std::to_wstring(i);
            out.push_back(std::move(entry));
        }
        if (progress) progress(out);
    }
    return true;
}
bool EnumerateRecycleBin(std::vector<DirEntry>&, RecycleBinInfo*, const RecycleProgress&) { return true; }
bool QueryDirectoryIdentity(const std::wstring&, DirectoryIdentity&) { return true; }
NetSnapshotWrite BeginNetSnapshotWrite(const std::wstring&) { return {}; }
bool SaveNetSnapshot(const NetSnapshotWrite&, const SnapshotPtr&) { return true; }
}
namespace pulse::app {
std::wstring FindGitRoot(const std::wstring&) { return {}; }
void ResolveLinksInPlace(const std::wstring&, std::vector<fs::DirEntry>&, const std::function<bool()>&) {}
ScopedEntryGrouping::ScopedEntryGrouping(int, const std::wstring&) {}
ScopedEntryGrouping::~ScopedEntryGrouping() = default;
bool EntryLess(const fs::DirEntry&, const fs::DirEntry&, ui::SortColumn, ui::SortDirection) { return false; }
void SortEntriesBySize(std::vector<fs::DirEntry>&, ui::SortDirection, const FolderSizeLookup&, const std::function<void()>&) {}
void SortEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir, const std::function<void()>& tick) {
    std::sort(entries.begin(), entries.end(), [&](const fs::DirEntry& a, const fs::DirEntry& b) { if (tick) tick(); return EntryLess(a, b, col, dir); });
}
}
namespace pulse::diagnostics::runtime {
void Event(const char*, std::initializer_list<Field>, Level) noexcept {}
}
static int failures = 0;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
static bool Drained() {
    for (int i = 0; i < 1000 && pulse::fs::active_network_enumerations; ++i) Sleep(2);
    return pulse::fs::active_network_enumerations == 0;
}
int main() {
    using namespace pulse;
    blocked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_io = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    delivered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        app::WorkerPool worker;
        worker.Start([](app::WorkResult) { ++callbacks; SetEvent(delivered); });
        auto generation = worker.Refresh(L"\\\\server\\blocked", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(blocked, 2000) == WAIT_OBJECT_0, "WorkerPool reaches blocked enumeration");
        worker.CancelGeneration(generation);
        worker.Refresh(L"C:\\healthy", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(delivered, 1000) == WAIT_OBJECT_0, "replacement navigation completes while old provider blocked");
        const auto start = GetTickCount64();
        worker.Stop();
        Check(GetTickCount64() - start < 1000, "Stop returns while provider ignores cancellation");
        Check(callbacks == 1, "only replacement result delivered");
    }
    SetEvent(release_io);
    Check(Drained(), "provider retires safely after WorkerPool destruction");
    Check(callbacks == 1, "late completion cannot call destroyed pool");
    ResetEvent(release_io); ResetEvent(blocked); ResetEvent(delivered);
    callbacks = 0; block_count = 0;
    {
        app::WorkerPool worker;
        worker.Start([](app::WorkResult) { ++callbacks; SetEvent(delivered); });
        for (int i = 0; i < 4; ++i)
            worker.Refresh(L"\\\\server\\blocked" + std::to_wstring(i), ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto waiting = GetTickCount64();
        const unsigned expected = std::min(4u, std::max(2u, std::thread::hardware_concurrency())) - 1;
        while (block_count < static_cast<int>(expected) && GetTickCount64() - waiting < 2000) Sleep(2);
        Check(block_count == static_cast<int>(expected), "network concurrency reserves one navigation worker");
        worker.Refresh(L"C:\\healthy", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(delivered, 1000) == WAIT_OBJECT_0, "queued local navigation bypasses blocked UNC workers");
        const auto start = GetTickCount64();
        worker.Stop();
        Check(GetTickCount64() - start < 1000, "Stop cancels concurrent and queued generations");
    }
    SetEvent(release_io);
    Check(Drained() && callbacks == 1, "concurrent late results discarded and reclaimed");
    {
        // A folder is read on a provider thread and only copied into the worker's own
        // vector once the scan ends, so the progressive rows have to come from the
        // batch the enumerator reports. Reading the worker's vector instead published
        // nothing and left every big folder on its skeleton until the end.
        app::WorkerPool worker;
        std::mutex mutex;
        std::vector<app::WorkResult> results;
        worker.Start([&](app::WorkResult result) {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(std::move(result));
        });
        worker.Refresh(L"C:\\streaming", ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto waiting = GetTickCount64();
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (results.size() >= 2) break;
            }
            if (GetTickCount64() - waiting >= 3000) break;
            Sleep(5);
        }
        size_t partial_rows = 0, final_rows = 0;
        bool partial_first = false, final_last = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (results.size() >= 2) {
                partial_first = results.front().partial;
                final_last = !results.back().partial;
                partial_rows = results.front().snapshot ? results.front().snapshot->size() : 0;
                final_rows = results.back().snapshot ? results.back().snapshot->size() : 0;
            }
        }
        Check(partial_first && final_last,
            "a long scan publishes gathered rows before the final result");
        Check(partial_rows == 2500 && final_rows == 3000,
            "the partial result carries the batch the enumerator reported, not the empty worker vector");
        worker.Stop();
    }
    CloseHandle(blocked); CloseHandle(release_io); CloseHandle(delivered);
    return failures ? 1 : 0;
}
