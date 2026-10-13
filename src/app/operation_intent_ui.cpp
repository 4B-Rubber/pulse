#include "operation_intent_ui.h"
#include "app_navigation.h"
#include "create_rename_intent.h"
#include <algorithm>

namespace pulse {
void ApplyOperationIntentCompletion(AppState& state, const ops::CompletedOperation& completed) {
    if (completed.refresh_only || !completed.task_id ||
        (completed.type != ops::OpType::CreateFolder && completed.type != ops::OpType::CreateTextFile)) return;
    std::vector<std::wstring> parents;
    ForEachPane(state, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab) return;
        for (const auto& destination : completed.destinations) {
            if (app::CompleteCreateRenameIntent(tab->create_rename_intents, completed.task_id,
                    tab->current_path, tab->view_generation, destination) &&
                std::find(parents.begin(), parents.end(), tab->current_path) == parents.end())
                parents.push_back(tab->current_path);
        }
    });
    // A watcher may already have delivered the new item while its owner was
    // unfocused. Request a fresh identity-checked snapshot for that owner too.
    for (const auto& parent : parents) RefreshPath(state, parent, RefreshReason::OperationCompleted);
}

void FinishOperationIntents(AppState& state, const ops::FinishedTask& finished) {
    state.tray.CompleteTask(finished.task_id, finished.moved_sources);
    ForEachPane(state, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab())
            app::FinishCreateRenameIntent(tab->create_rename_intents, finished.task_id);
    });
}
}
