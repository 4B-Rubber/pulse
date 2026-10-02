#pragma once
// File Explorer "Pulse tags >" context submenu (static HKCU verbs).
//
// Explorer launches `pulse.exe --tag <TagId> "<path>"` once per selected item.
// Secondary processes forward the request to the running instance, which
// batches everything that arrives within a short window per tag and applies
// ToggleTagForSelection semantics: remove when every item already has the
// tag, otherwise add it to all of them. When Pulse was not running, the
// launched process stays hidden and exits once the batch is written.

#include <windows.h>

#include <optional>
#include <string>

namespace pulse {
struct AppState;
}

namespace pulse::app {

struct ShellTagRequest {
    std::wstring tag_id;
    std::wstring path;
};

// `--tag <id> <path>` anywhere on the command line.
std::optional<ShellTagRequest> ParseShellTagArgs(int argc, wchar_t** argv);
// Sends the request to the running Pulse window (waits briefly for it).
bool ForwardShellTagRequest(const ShellTagRequest& request);
bool DecodeShellTagRequest(const COPYDATASTRUCT* data, ShellTagRequest& out);

} // namespace pulse::app

namespace pulse {

// headless_launch: this process was started only to apply the tag.
void QueueShellTagRequest(AppState& s, app::ShellTagRequest request, bool headless_launch);
bool ShellTagHeadlessLaunch();
// Called from the UI timer: applies due batches, keeps the registry menu in
// sync with the tag list and the setting, and ends a headless launch.
void TickShellTagMenu(AppState& s, ULONGLONG now);

} // namespace pulse
