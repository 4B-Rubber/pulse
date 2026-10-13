#ifdef PULSE_WITH_SELFTEST
#include "operation_intent_ui.h"
#include "app_navigation.h"
#include "app_hosted_edit.h"
#include "create_rename_intent.h"
#include <cstdio>
#include <filesystem>

bool RunOperationIntentUiTest() {
    using namespace pulse;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/m12023_operation_intent_ui_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log); failures += !ok;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true; s.shot.active = true;
    s.appPrefs.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 900, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.window_tabs.NewTab(L"C:\\operation-intent-origin");
    auto* origin = s.window_tabs.Active()->panes.front()->ActiveTab();
    s.window_tabs.NewTab(L"C:\\operation-intent-other");
    BindCurrentLayout(s);
    auto* other = ActiveTab(s);
    app::QueueCreateRenameIntent(origin->create_rename_intents, 71, origin->current_path, origin->view_generation);
    app::QueueCreateRenameIntent(origin->create_rename_intents, 72, origin->current_path, origin->view_generation);
    app::QueueCreateRenameIntent(other->create_rename_intents, 73, other->current_path, other->view_generation);
    ops::CompletedOperation success;
    success.type = ops::OpType::CreateTextFile; success.task_id = 71;
    success.sources = success.destinations = {origin->current_path + L"\\same.txt"};
    ApplyOperationIntentCompletion(s, success);
    auto pending = app::PendingCreateRenameIntent(origin->create_rename_intents, origin->current_path, origin->view_generation);
    const bool exact_identity = pending && pending->task_id == 71 && pending->ready &&
        pending->view_generation == origin->view_generation &&
        fs::NormalizePath(pending->parent) == fs::NormalizePath(origin->current_path) &&
        fs::NormalizePath(pending->actual_path) == fs::NormalizePath(success.destinations.front());
    if (!exact_identity) fprintf(log, "[INFO] task=%llu parent=%ls actual=%ls expected_parent=%ls expected_actual=%ls\n",
        static_cast<unsigned long long>(pending ? pending->task_id : 0),
        pending ? pending->parent.c_str() : L"<none>", pending ? pending->actual_path.c_str() : L"<none>",
        origin->current_path.c_str(), success.destinations.front().c_str());
    check(exact_identity,
        "completion arms exact created path on its unfocused owner");
    check(origin->pending_generation != 0 && other->pending_generation == 0,
        "completion queues only origin refresh even when another tab is focused");
    check(origin->SelectedCount() == 0 && other->SelectedCount() == 0 && s.renameIndex == -1,
        "completion alone cannot select a same-name item or open another tab editor");
    FinishOperationIntents(s, ops::FinishedTask{71, {}});
    check(app::PendingCreateRenameIntent(origin->create_rename_intents, origin->current_path, origin->view_generation).has_value(),
        "success remains armed when the matching terminal is processed later");
    FinishOperationIntents(s, ops::FinishedTask{72, {}});
    check(origin->create_rename_intents.size() == 1 && other->create_rename_intents.size() == 1,
        "failed terminal removes only its own unresolved request");
    success.task_id = 73; success.destinations = {origin->current_path + L"\\same.txt"};
    ApplyOperationIntentCompletion(s, success);
    check(other->create_rename_intents.empty() && other->pending_generation == 0,
        "mismatched completion parent never refreshes or arms another directory");
    s.window_tabs.CloseTab(0);
    BindCurrentLayout(s);
    success.task_id = 71;
    ApplyOperationIntentCompletion(s, success);
    FinishOperationIntents(s, ops::FinishedTask{71, {}});
    check(ActiveTab(s) == other && other->create_rename_intents.empty() && other->pending_generation == 0,
        "late success and terminal after origin close cannot affect surviving tab");

    auto listing = [&](std::initializer_list<const wchar_t*> names) {
        auto snapshot = std::make_shared<std::vector<fs::DirEntry>>();
        for (const auto* name : names) {
            fs::DirEntry entry;
            entry.name = name;
            entry.full_path = other->current_path + L"\\" + name;
            entry.attrs = FILE_ATTRIBUTE_NORMAL;
            snapshot->push_back(std::move(entry));
        }
        return snapshot;
    };
    auto deliver = [&](const fs::SnapshotPtr& snapshot) {
        app::WorkResult result{};
        result.path = other->current_path;
        result.generation = other->pending_generation;
        result.snapshot = snapshot;
        check(result.generation != 0, "editor test delivers a requested snapshot generation");
        ApplyWorkerResult(s, result);
    };
    auto edit_text = [&] {
        wchar_t value[256]{};
        if (s.hwndRenameEdit) GetWindowTextW(s.hwndRenameEdit, value, ARRAYSIZE(value));
        return std::wstring(value);
    };
    auto editing = [&](std::wstring_view name) {
        return s.renameIndex >= 0 && static_cast<size_t>(s.renameIndex) < other->EntryCount() &&
            other->EntryAt(s.renameIndex).name == name;
    };
    other->SetSnapshot(listing({L"A.txt"}));
    other->SelectOnly(0);
    ShowRenameOverlay(s);
    check(IsWindow(s.hwndRenameEdit) && s.renameIndex == 0, "production rename creates a real EDIT for A");
    if (IsWindow(s.hwndRenameEdit) && s.renameIndex == 0) {
        SetWindowTextW(s.hwndRenameEdit, L"A unfinished draft.txt");
        app::QueueCreateRenameIntent(other->create_rename_intents, 81, other->current_path, other->view_generation);
        success.task_id = 81;
        success.sources = success.destinations = {other->current_path + L"\\B.txt"};
        ApplyOperationIntentCompletion(s, success);
        const auto changed = listing({L"B.txt", L"A.txt"});
        deliver(changed);
        check(editing(L"A.txt") &&
            edit_text() == L"A unfinished draft.txt", "B completion snapshot preserves A editor target and unfinished text");
        check(other->create_rename_intents.size() == 1 && other->create_rename_intents.front().task_id == 81,
            "snapshot while A edits does not consume B create intent");
        HideRenameOverlay(s, false);
        check(s.renameIndex == -1 && other->pending_generation != 0 && other->create_rename_intents.size() == 1,
            "ending A queues refresh without opening B synchronously");
        deliver(changed);
        check(editing(L"B.txt") && edit_text() == L"B.txt" &&
            other->create_rename_intents.empty(), "fresh valid snapshot opens B with its own text and consumes only B");

        app::QueueCreateRenameIntent(other->create_rename_intents, 82, other->current_path, other->view_generation);
        app::QueueCreateRenameIntent(other->create_rename_intents, 83, other->current_path, other->view_generation);
        success.task_id = 82; success.sources = success.destinations = {other->current_path + L"\\missing.txt"};
        ApplyOperationIntentCompletion(s, success);
        success.task_id = 83; success.sources = success.destinations = {other->current_path + L"\\C.txt"};
        ApplyOperationIntentCompletion(s, success);
        HideRenameOverlay(s, false);
        deliver(listing({L"A.txt", L"B.txt", L"C.txt"}));
        check(editing(L"C.txt") && edit_text() == L"C.txt",
            "production snapshot skips missing ready item and opens later existing C");
        check(other->create_rename_intents.size() == 1 && other->create_rename_intents.front().task_id == 82,
            "opening C keeps missing request identity separate");
    }
    other->create_rename_intents.clear();
    HideRenameOverlay(s, false);
    s.worker.Stop();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    s.hwndRenameEdit = nullptr;
    if (s.editFont) { DeleteObject(s.editFont); s.editFont = nullptr; }
    if (s.editBrush) { DeleteObject(s.editBrush); s.editBrush = nullptr; }
    fprintf(log, "failures=%d\n", failures); fclose(log);
    return failures == 0;
}
#endif
