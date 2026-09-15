#pragma once
#include <functional>
#include <optional>
#include <cstdint>
#include <string>
namespace pulse {
namespace app { struct Tab; }
struct AppState;
struct ContentSizeSummary;
std::optional<uint64_t> ContentSelectionSize(app::Tab& tab);
void SelectContentPattern(AppState& s,const std::wstring& pattern);
struct ContentSelectionAction;
bool RefreshContentResults(AppState& s);
void CancelContentSelection(AppState& s, const app::Tab& tab);
bool DeferContentSelection(AppState& s, std::function<void(AppState&)> action, bool focused_only=false);
void CompleteContentSelection(AppState& s);
}
