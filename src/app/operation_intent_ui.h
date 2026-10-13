#pragma once

namespace pulse {
struct AppState;
namespace ops { struct CompletedOperation; struct FinishedTask; }
void ApplyOperationIntentCompletion(AppState& state, const ops::CompletedOperation& completed);
void FinishOperationIntents(AppState& state, const ops::FinishedTask& finished);
}
