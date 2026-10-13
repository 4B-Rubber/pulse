#pragma once
// File Explorer "Pulse tags >" context submenu (static HKCU verbs).
//
// DelegateExecute supplies one complete Shell selection to the existing EXE.
// The legacy --tag command remains a single-file operation; requests are never
// grouped by timing. A cold COM launch uses the normal app instance, hidden.

// Compatibility --tag-add and --tag-remove requests use explicit actions. Requests keep their IDs across forwarding retries;
// arrival timing never decides whether a tag is added or removed. Legacy
// --tag requests mean add. A hidden launch exits after its writes drain.
#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace pulse {
struct AppState;
}

namespace pulse::app {

enum class ShellTagAction { Add, Remove };
struct ShellTagRequest {
    std::wstring tag_id;
    std::wstring path;
    ShellTagAction action = ShellTagAction::Add;
    std::wstring operation_id;
};

// Parses --tag-add/--tag-remove <id> <path>; --tag is the legacy add alias.
std::optional<ShellTagRequest> ParseShellTagArgs(int argc, wchar_t** argv);
// Sends the request to the running Pulse window (waits briefly for it).
bool ForwardShellTagRequest(const ShellTagRequest& request, std::wstring_view endpoint_name = {});
std::wstring EncodeShellTagRequest(const ShellTagRequest& request);
ULONG_PTR ShellTagMessageId();
bool DecodeShellTagRequest(const COPYDATASTRUCT* data, ShellTagRequest& out);

} // namespace pulse::app

namespace pulse {

// headless_launch: this process was started only to apply the tag.
void QueueShellTagRequest(AppState& s, app::ShellTagRequest request, bool headless_launch);
bool ShellTagHeadlessLaunch();
void BeginShellTagComLaunch();
// Called from the UI timer: applies due batches, keeps the registry menu in
// sync with the tag list and the setting, and ends a headless launch.
void TickShellTagMenu(AppState& s, ULONGLONG now);

} // namespace pulse
