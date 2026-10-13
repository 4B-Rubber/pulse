#include "../app/places.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <iostream>
#include <atomic>

namespace {
HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
HANDLE released = CreateEventW(nullptr, TRUE, FALSE, nullptr);
HANDLE dirty_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
std::atomic<unsigned> blocked_stage{0};
std::atomic<bool> watch_dirty{false};
int failures = 0;
void Check(bool value, const char* text) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << text << '\n';
    if (!value) ++failures;
}
void Arm(unsigned stage) {
    ResetEvent(entered); ResetEvent(released); ResetEvent(dirty_entered); blocked_stage = stage;
}
void Release() { SetEvent(released); }
}
namespace pulse::app {
std::wstring fixture;
std::wstring GetPulseDataDir() { return fixture; }
void PlacesSaveTestStage(unsigned stage) {
    if (stage == 3 && watch_dirty) SetEvent(dirty_entered);
    unsigned expected = stage;
    if (blocked_stage.compare_exchange_strong(expected, 0)) {
        SetEvent(entered);
        WaitForSingleObject(released, 5000);
    }
}
struct PlacesSaveTestAccess {
    static void Queue(const PlacesCatalog& value) { value.QueuePlacesSave(); }
    static void Stop(PlacesCatalog& value) { value.StopPlacesWriter(); }
    static bool Dirty(const PlacesCatalog& value) {
        std::lock_guard lock(value.places_save_mutex_);
        return value.places_save_due_ != 0;
    }
};
}
int wmain() {
    using namespace pulse::app;
    namespace fs = std::filesystem;
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto base = parent / (L"places-save-order-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(base)) return 1;
    const auto old = (base / L"old.txt").wstring(), added = (base / L"added.txt").wstring();
    for (int scenario = 0; scenario < 4; ++scenario) {
        const auto dir = base / std::to_wstring(scenario); fs::create_directory(dir); fixture = dir.wstring();
        {
            PlacesCatalog catalog;
            if (scenario == 1) catalog.ToggleStarred(old, PlaceItemKind::File);
            catalog.RecordRecent(old, PlaceItemKind::File);
            Arm(1); PlacesSaveTestAccess::Queue(catalog);
            const bool paused = WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0;
            Check(paused, "old background snapshot dequeued before synchronous save");
            if (scenario == 0) catalog.ClearRecent();
            if (scenario == 1) catalog.ToggleStarred(old, PlaceItemKind::File);
            if (scenario == 2) catalog.SetQuickAccessPinned({added}, true);
            if (scenario == 3) { catalog.recent_items.clear(); Check(catalog.Save(), "final lifecycle Save persists newest catalog"); }
            Release(); PlacesSaveTestAccess::Stop(catalog);
        }
        PlacesCatalog loaded; loaded.persist = false;
        const bool valid = loaded.Load() && (scenario == 0 || scenario == 3 ? loaded.recent_items.empty() :
            scenario == 1 ? !loaded.IsStarred(old) : loaded.IsQuickAccessPinned(added));
        const char* names[] = {"old writer cannot restore synchronously cleared recent items", "old writer cannot restore removed favorite",
            "old writer cannot erase new quick access pin", "writer drained after final Save cannot overwrite it"};
        Check(valid, names[scenario]);
    }
    fixture = (base / L"completion").wstring(); fs::create_directory(fixture);
    {
        PlacesCatalog catalog;
        catalog.RecordRecent(old, PlaceItemKind::File);
        Arm(2); PlacesSaveTestAccess::Queue(catalog);
        Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "writer pauses between generation check and dirty acknowledgement");
        watch_dirty = true;
        HANDLE changed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::jthread mutation([&] { catalog.RecordRecent(added, PlaceItemKind::File); SetEvent(changed); });
        Check(WaitForSingleObject(dirty_entered, 3000) == WAIT_OBJECT_0, "new mutation reaches dirty publication during acknowledgement");
        Check(WaitForSingleObject(changed, 100) == WAIT_TIMEOUT, "generation check and dirty clear hold one state lock");
        Release(); mutation.join(); watch_dirty = false; CloseHandle(changed);
        PlacesSaveTestAccess::Stop(catalog);
        Check(PlacesSaveTestAccess::Dirty(catalog), "newer mutation retains pending save after old completion");
        Check(catalog.FlushPendingSave(true), "newer dirty state flushes successfully");
        PlacesCatalog loaded; loaded.persist = false;
        Check(loaded.Load() && loaded.recent_items.size() == 2, "both recent items survive completion race");
    }
    fixture = (base / L"retry").wstring(); fs::create_directory(fixture);
    {
        PlacesCatalog catalog;
        catalog.RecordRecent(old, PlaceItemKind::File); Check(catalog.Save(), "retry baseline written");
        HANDLE locked = CreateFileW((fixture + L"\\places.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "exclusive file handle injects real persistence failure");
        catalog.RecordRecent(added, PlaceItemKind::File);
        Check(!catalog.Save() && PlacesSaveTestAccess::Dirty(catalog), "failed synchronous write retains dirty generation");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        Check(catalog.FlushPendingSave(true) && !PlacesSaveTestAccess::Dirty(catalog), "unlocked synchronous failure retries newest state");
        HANDLE locked_again = CreateFileW((fixture + L"\\places.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        catalog.RecordRecent((base / L"third.txt").wstring(), PlaceItemKind::File);
        PlacesSaveTestAccess::Queue(catalog); PlacesSaveTestAccess::Stop(catalog);
        Check(PlacesSaveTestAccess::Dirty(catalog), "failed background write retains pending state");
        if (locked_again != INVALID_HANDLE_VALUE) CloseHandle(locked_again);
    }
    {
        PlacesCatalog loaded; loaded.persist = false;
        Check(loaded.Load() && loaded.recent_items.size() == 3, "destructor drain retries failed worker with final snapshot");
    }
    CloseHandle(entered); CloseHandle(released); CloseHandle(dirty_entered);
    if (base.parent_path() != parent || !base.filename().wstring().starts_with(L"places-save-order-")) return 1;
    fs::remove_all(base);
    return failures ? 1 : 0;
}
