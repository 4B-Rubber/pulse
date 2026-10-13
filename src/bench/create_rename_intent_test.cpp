#include "../app/create_rename_intent.h"
#include "../ops/ops_manager.h"
#include <windows.h>
#include <filesystem>
#include <cstdio>
#include <algorithm>

using namespace pulse;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
struct Fixture {
    std::filesystem::path root;
    bool owned = false;
    Fixture() {
        const auto base = std::filesystem::absolute(L"bench_data");
        std::error_code error;
        std::filesystem::create_directories(base, error);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            root = base / (L"create-intent-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (CreateDirectoryW(root.c_str(), nullptr)) { owned = true; break; }
            if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
    }
    ~Fixture() {
        // This exact root was exclusively created above; never remove an existing fixture.
        if (owned) { std::error_code error; std::filesystem::remove_all(root, error); }
    }
};
using Intents = std::vector<app::CreateRenameIntent>;
bool Wait(ops::OpsManager& manager, uint64_t task_id, Intents& intents, const std::wstring& parent) {
    const auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        const auto finished = manager.DrainFinishedTasks();
        const auto successes = manager.DrainCompletions();
        for (const auto& success : successes) for (const auto& destination : success.destinations)
            app::CompleteCreateRenameIntent(intents, success.task_id, parent, 1, destination);
        bool done = false;
        for (const auto& terminal : finished) {
            app::FinishCreateRenameIntent(intents, terminal.task_id);
            done |= terminal.task_id == task_id;
        }
        if (done) return true;
        Sleep(5);
    }
    return false;
}
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring a = L"C:\\origin", b = L"C:\\other";
    Intents origin, other;
    app::QueueCreateRenameIntent(origin, 10, a, 1);
    Check(!app::PendingCreateRenameIntent(origin, a, 1), "snapshot before success cannot arm rename");
    Check(!app::CompleteCreateRenameIntent(other, 10, b, 1, b + L"\\same.txt") && other.empty(),
        "other pane with same basename cannot claim origin task");
    Check(app::CompleteCreateRenameIntent(origin, 10, a, 1, a + L"\\same.txt"), "success arms only origin");
    app::FinishCreateRenameIntent(origin, 10);
    auto ready = app::PendingCreateRenameIntent(origin, a, 1);
    Check(ready.has_value(), "success remains ready after terminal notification");
    Check(ready && !app::CreateRenameEntryMatches(*ready, L"same.txt", b + L"\\same.txt"),
        "same basename from another full path never matches");
    Check(ready && !app::CreateRenameEntryMatches(*ready, L"different.txt", {}), "missing target cannot consume intent");
    Check(app::PendingCreateRenameIntent(origin, a, 1).has_value(), "background snapshot retains origin ready intent");
    Check(ready && app::CreateRenameEntryMatches(*ready, L"same.txt", {}), "ordinary snapshot name resolves under bound parent");
    app::ConsumeCreateRenameIntent(origin, 10);
    Check(origin.empty(), "selected successful item consumes only its request");

    app::QueueCreateRenameIntent(origin, 20, a, 1);
    app::QueueCreateRenameIntent(origin, 21, a, 1);
    app::CompleteCreateRenameIntent(origin, 21, a, 1, a + L"\\second.txt");
    app::FinishCreateRenameIntent(origin, 20);
    ready = app::PendingCreateRenameIntent(origin, a, 1);
    Check(ready && ready->task_id == 21, "first failed create cannot erase second successful create");
    app::QueueCreateRenameIntent(origin, 22, a, 1);
    app::CompleteCreateRenameIntent(origin, 22, a, 1, a + L"\\third.txt");
    app::ConsumeCreateRenameIntent(origin, 21);
    ready = app::PendingCreateRenameIntent(origin, a, 1);
    Check(ready && ready->task_id == 22, "two successful creates retain separate identities");
    const app::CreateRenameEntry both_entries[] = {{L"second.txt", {}}, {L"third.txt", {}}};
    Check(!app::SelectCreateRenameIntent(origin, a, 1, both_entries, true, true) &&
        origin.size() == 1 && origin.front().task_id == 22,
        "active rename editor defers next create without consuming its identity");
    auto selected = app::SelectCreateRenameIntent(origin, a, 1, both_entries, true, false);
    Check(selected && selected->task_id == 22 && selected->entry_index == 1,
        "after editor ends fresh snapshot can select deferred create");
    app::QueueCreateRenameIntent(origin, 23, a, 1);
    app::CompleteCreateRenameIntent(origin, 23, a, 1, a + L"\\fourth.txt");
    const app::CreateRenameEntry missing_first[] = {{L"fourth.txt", {}}};
    selected = app::SelectCreateRenameIntent(origin, a, 1, missing_first, true, false);
    Check(selected && selected->task_id == 23 && origin.size() == 2,
        "missing first ready item does not block later existing successful item");
    if (selected) app::ConsumeCreateRenameIntent(origin, selected->task_id);
    Check(origin.size() == 1 && origin.front().task_id == 22,
        "selecting later item does not discard missing item's separate intent");
    Check(!app::SelectCreateRenameIntent(origin, a, 1, both_entries, false, false),
        "background pane snapshot cannot select even a matching successful create");
    Check(!app::PendingCreateRenameIntent(origin, b, 1) && origin.empty(), "navigation invalidates even ready intent");
    Check(!app::PendingCreateRenameIntent(origin, a, 1), "returning to origin cannot revive navigation-cancelled intent");
    app::QueueCreateRenameIntent(origin, 30, a, 1);
    Check(!app::CompleteCreateRenameIntent(origin, 30, a, 2, a + L"\\same.txt") && origin.empty(),
        "changed view generation rejects delayed success");
    app::QueueCreateRenameIntent(origin, 31, a, 1);
    Check(!app::CompleteCreateRenameIntent(origin, 31, a, 1, b + L"\\same.txt") && origin.empty(),
        "success path outside requested parent cancels intent");
    app::QueueCreateRenameIntent(origin, 32, a, 1);
    app::FinishCreateRenameIntent(origin, 32);
    Check(origin.empty(), "cancelled task terminal clears unarmed intent");
    { Intents closed; app::QueueCreateRenameIntent(closed, 33, a, 1); }
    Check(!app::CompleteCreateRenameIntent(other, 33, a, 1, a + L"\\same.txt"), "closed origin cannot transfer request to another tab");
    app::QueueCreateRenameIntent(origin, 40, L"\\\\?\\C:\\origin\\", 1);
    Check(app::CompleteCreateRenameIntent(origin, 40, a, 1, L"c:\\ORIGIN\\same.txt"),
        "extended prefix trailing separator and case preserve parent identity");
    origin.clear();

    Fixture fixture;
    Check(fixture.owned, "exclusive private fixture created");
    if (!fixture.owned) return 1;
    const auto parent = fixture.root.wstring();
    const auto other_parent = fixture.root / L"other-pane";
    Check(CreateDirectoryW(other_parent.c_str(), nullptr) != FALSE, "private other-pane folder created");
    ops::OpsManager manager;
    manager.Start([] {});
    for (const bool folder : {false, true}) {
        const auto destination = fixture.root / (folder ? L"created-folder" : L"created.txt");
        ops::OpRequest request;
        request.type = folder ? ops::OpType::CreateFolder : ops::OpType::CreateTextFile;
        request.sources = {destination.wstring()};
        const auto task = manager.Submit(std::move(request));
        app::QueueCreateRenameIntent(origin, task, parent, 1);
        Check(!app::PendingCreateRenameIntent(origin, parent, 1), "real Submit does not arm rename before consumer completion");
        Check(Wait(manager, task, origin, parent), "real create terminal arrives within deadline");
        ready = app::PendingCreateRenameIntent(origin, parent, 1);
        Check(std::filesystem::exists(destination) && ready && ready->task_id == task &&
            app::CreateRenameEntryMatches(*ready, destination.filename().wstring(), destination.wstring()),
            "real create success supplies actual target to production rename helper");
        const auto other_destination = other_parent / destination.filename();
        if (folder) CreateDirectoryW(other_destination.c_str(), nullptr);
        else {
            const auto file = CreateFileW(other_destination.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
        Check(std::filesystem::exists(other_destination) && ready &&
            !app::CreateRenameEntryMatches(*ready, other_destination.filename().wstring(), other_destination.wstring()),
            "real same-name item in another private directory cannot receive rename");
        app::ConsumeCreateRenameIntent(origin, task);
    }
    ops::OpRequest failed;
    failed.type = ops::OpType::CreateTextFile;
    failed.sources = {(fixture.root / L"missing-parent" / L"cannot-create.txt").wstring()};
    const auto failed_task = manager.Submit(std::move(failed));
    app::QueueCreateRenameIntent(origin, failed_task, parent, 1);
    Check(Wait(manager, failed_task, origin, parent), "real failed create terminal arrives");
    Check(origin.empty(), "real failed create cannot leave a future rename request");
    manager.Stop();
    return failures ? 1 : 0;
}
