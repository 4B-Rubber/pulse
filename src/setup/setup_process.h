#pragma once
// Closing running Pulse before files are replaced. Port of the
// "closing running Pulse" section of installer/PulseSetup.iss.

#include <windows.h>

#include <string>
#include <vector>

namespace pulse::setup {

enum class CloseState { Done = 0, Failed = 1, Busy = 2 };

// Asks every Pulse (window class PulseMainWindow) whose exe lives in `dirs` to
// exit with the Pulse.PrepareUpdateShutdown.v1 handshake. Unattended updates
// never terminate a UI process that did not acknowledge; they keep waiting
// while it reports busy, exactly like the Inno script.
CloseState ClosePulseForUpdate(const std::vector<std::wstring>& dirs, bool unattended_update);

// Stops the PulseIndex service (when it runs from `dirs`) and ends
// Index, preview, Shell, AI and content/document hosts from `dirs`.
void StopPulseHosts(const std::vector<std::wstring>& dirs, bool touch_service);
bool WaitUntilImageGone(const wchar_t* image, const std::vector<std::wstring>& dirs, unsigned timeout_ms);

// True when `pid` still runs an image outside `dirs` (a third-party host such
// as QQ that mapped the dialog DLL). False for an exited or unreadable process:
// those keep the conservative "ask the user" path.
bool ProcessOutsideDirectories(DWORD pid, const std::vector<std::wstring>& dirs);

}  // namespace pulse::setup
