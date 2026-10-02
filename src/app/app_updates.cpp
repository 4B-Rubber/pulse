#include "app_updates.h"
#include "app_state.h"
#include "app_internal.h"
#include "pulse_version.h"
#include "../common/localization.h"
#include <cstdio>

namespace pulse {
namespace {
constexpr ULONGLONG kCheckInterval = 6ull * 60 * 60 * 1000;
void ShowInstallError(AppState& state) {
    using l10n::StringId;
    const auto message = state.update_install_error == ERROR_CANCELLED ? StringId::UpdateCancelled :
        state.update_install_error == ERROR_BUSY ? StringId::UpdateBusy : StringId::UpdateInstallFailed;
    state.notification_toast.Show(state.hwnd, l10n::Get(StringId::Update), l10n::Get(message));
}
}

void CheckForUpdates(AppState& state) {
    // Deliberately ignores the automatic-check switch: this is the manual path, and a user who
    // silenced the background check must still be able to look for an update by hand.
    if (state.update_installer.downloading() || state.update_installer.installing()) return;
    if (state.update_checker.CheckAsync(state.hwnd, WM_UPDATE_RESULT)) {
        state.update_result_ready = false;
        state.update_install_error = ERROR_SUCCESS;
        state.next_update_check = GetTickCount64() + kCheckInterval;
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
}

void TickUpdates(AppState& state, unsigned long long now) {
    DWORD install_error = ERROR_SUCCESS;
    if (state.update_installer.TakeInstallResult(install_error)) {
        state.update_install_error = install_error;
        if (install_error) ShowInstallError(state);
        state.update_installer.Stop();
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
    // Snapshot reads are cheap; repaint at most 10 Hz, only while work is active and visible.
    // Bytes are never posted as individual window messages.
    if (!state.shot.active && state.update_installer.Progress().active() &&
        now >= state.next_update_progress_paint && IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd)) {
        state.next_update_progress_paint = now + 100;
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
    if (!state.whatsNewVersion.empty() && now >= state.whatsNewAt &&
        IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd)) {
        wchar_t title[160]{};
        swprintf_s(title, l10n::Get(l10n::StringId::UpdatedTitleFormat).c_str(),
                   state.whatsNewVersion.c_str());
        state.notification_toast.Show(state.hwnd, title, l10n::Get(l10n::StringId::UpdatedClick),
                                      false, WM_SHOW_RELEASE_NOTES);
        state.whatsNewVersion.clear();
    }
    // The switch silences the background check and the "update available" reminder. It must not
    // cancel work the user started by hand: a manual check may still be in flight, and a download
    // or install launched from the card is allowed to finish. The "what's new" toast above is part
    // of finishing an update, so it stays in front of this gate.
    if (!state.appPrefs.check_updates) {
        state.notified_update_version.clear();
        return;
    }
    if (state.shot.active || !app::UpdateChecker::Enabled() || now < state.next_update_check) return;
    if (state.update_installer.downloading() || state.update_installer.installing() || state.update_checker.checking()) return;
    state.next_update_check = now + kCheckInterval;
    CheckForUpdates(state);
}

void NoteRunningVersion(AppState& state) {
    auto& prefs = state.appPrefs;
    if (!prefs.persist || state.shot.active || state.menushot) return;
    if (prefs.last_seen_version == PULSE_VERSION_STRING) return;
    // A fresh install has no app.json yet; only upgrades get the toast.
    if (prefs.had_file) {
        state.whatsNewVersion = PULSE_VERSION_STRING;
        state.whatsNewAt = GetTickCount64() + 2500;
    }
    prefs.last_seen_version = PULSE_VERSION_STRING;
    prefs.Save();
}

void ShowReleaseNotes(AppState& state) {
    OpenSettingsTab(state, 3);
    state.settingsReleaseExpanded = 0;
    const auto vm = BuildVm(state, false);
    const float w = static_cast<float>(state.compositor.Width());
    const float h = static_cast<float>(state.compositor.Height());
    state.settings.SetScroll(
        state.renderer.SettingsDestinationOffset(vm, static_cast<int>(l10n::StringId::ReleaseNotes), w, h),
        state.renderer.SettingsMaxScroll(vm, w, h));
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void InstallUpdate(AppState& state) {
    // Same as the check above: downloading and installing is the manual path and stays available
    // while the automatic-check switch is off.
    if (state.update_installer.installing()) return;
    if (state.update_installer.downloading()) {
        state.update_installer.Stop();
        state.update_install_error = ERROR_CANCELLED;
    } else if (state.update_result_ready && state.update_result.update_available) {
        state.update_install_error = ERROR_SUCCESS;
        if (!state.update_installer.Start(state.update_result, state.hwnd, WM_UPDATE_DOWNLOADED))
            state.update_install_error = ERROR_GEN_FAILURE;
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void CompleteUpdateCheck(AppState& state) {
    app::UpdateResult result;
    if (!state.update_checker.TakeResult(result)) return;
    state.update_result = std::move(result);
    state.update_result_ready = true;
    if (state.update_result.update_available && state.appPrefs.check_updates &&
        state.notified_update_version != state.update_result.version) {
        state.notified_update_version = state.update_result.version;
        wchar_t title[160]{};
        swprintf_s(title, l10n::Get(l10n::StringId::UpdateAvailableFormat).c_str(),
            state.update_result.version.c_str());
        state.notification_toast.Show(state.hwnd, title,
            l10n::Get(l10n::StringId::UpdateClickToInstall), true, WM_UPDATE_INSTALL);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void CompleteUpdateDownload(AppState& state) {
    DWORD error = ERROR_SUCCESS;
    if (!state.update_installer.TakeResult(error)) return;
    // Downloads only start from the card's buttons, so the automatic-check switch has nothing to
    // say about finishing one: installing what the user asked for is exactly what they asked for.
    if (!error && (state.ops.Status().active || state.settings.migration_pending())) error = ERROR_BUSY;
    if (!error) {
        // Paint the verified/starting stage before ShellExecute can enter an elevation prompt.
        InvalidateRect(state.hwnd, nullptr, FALSE);
        UpdateWindow(state.hwnd);
        state.update_installer.Launch(state.hwnd, error);
    }
    state.update_install_error = error;
    if (error) {
        state.update_installer.Stop();
        ShowInstallError(state);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}
}
