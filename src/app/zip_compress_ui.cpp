// zip_compress_ui.cpp - see zip_compress_ui.h.
#include "zip_compress_ui.h"
#include "app_internal.h"
#include "app_messages.h"
#include "arrival_select.h"
#include "selection_caps.h"
#include "zip_compress.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/text_format.h"
#include "../ops/ops_manager.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace pulse {
namespace {

// Arrival tasks of 压缩 stay clear of the ops queue's ids.
constexpr uint64_t kZipTaskBase = 1ull << 62;


struct ZipDone {
    uint64_t id = 0;
    uint64_t task_id = 0;   // the operation window's task; 0 when the caller draws its own ring
    std::wstring zip_path;
    app::ZipCompressResult result;
};

// Posted while an archive runs so the operation window can show a live bar
// without the worker touching AppState.
struct ZipProgress {
    uint64_t task_id = 0;
    uint64_t done = 0;
    uint64_t total = 0;
    uint64_t items_done = 0;
    uint64_t items_total = 0;
    double bytes_per_second = 0.0;
    double peak_bytes_per_second = 0.0;
};

uint64_t NextZipId() {
    static uint64_t next = 0;
    return kZipTaskBase + ++next;
}

std::wstring Leaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Fill(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, wcslen(key), value);
    return text;
}

bool CompressTarget(AppState& s, std::vector<std::wstring>* paths, std::wstring* dir) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->SelectedCount() <= 0) return false;
    if (!SelectionCapsOf(s).copy) return false;
    std::vector<std::wstring> selected = SelectedFullPaths(*tab);
    if (selected.empty()) return false;
    // One parent (search results may mix folders): the archive goes there.
    const std::wstring parent = fs::ParentPath(selected.front());
    if (parent.empty() || fs::IsVirtualPath(parent)) return false;
    for (const auto& path : selected)
        if (fs::IsVirtualPath(path) || _wcsicmp(fs::ParentPath(path).c_str(), parent.c_str()) != 0) return false;
    if (paths) *paths = std::move(selected);
    if (dir) *dir = parent;
    return true;
}

void ShowResult(AppState& s, const ZipDone& done) {
    const std::wstring name = Leaf(done.zip_path);
    s.notification_toast.ReplaceNextSnack();   // the 正在压缩 capsule becomes the result
    if (done.result.ok) {
        std::wstring detail = Fill(l10n::Get(l10n::StringId::ZipCompressDetail), L"{n}",
                                   std::to_wstring(done.result.files));
        WIN32_FILE_ATTRIBUTE_DATA data{};
        uint64_t packed = 0;
        if (GetFileAttributesExW(done.zip_path.c_str(), GetFileExInfoStandard, &data))
            packed = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        detail = Fill(std::move(detail), L"{size}", format::ByteSize(packed));
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge,
            Fill(l10n::Get(l10n::StringId::ZipCompressDone), L"{name}", name), std::move(detail), {});
    } else if (done.result.cancelled) {
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge,
            l10n::Get(l10n::StringId::ZipCompressCancelled), name, {}, {}, 3000);
    } else {
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::ZipCompressFailed),
            done.result.error.empty() ? name : done.result.error);
    }
}

} // namespace

bool CanCompressSelection(AppState& s) {
    // Asked on every toolbar paint: the cheap part only; CompressSelection
    // checks each item.
    const app::Tab* tab = ActiveTab(s);
    return tab && tab->snapshot && tab->SelectedCount() > 0 && !tab->current_path.empty() &&
        !fs::IsVirtualPath(tab->current_path) && !tab->net_readonly && app::ZipCompressAvailable();
}

namespace {
// Shared by the toolbar tool and the staging shelf. With `progress` the
// caller shows its own indicator (the shelf's ring) instead of the busy snack.
uint64_t StartZip(AppState& s, std::vector<std::wstring> paths, const std::wstring& dir,
                  const std::wstring& zip_path, app::ArchiveOptions options,
                  std::shared_ptr<std::atomic<float>> progress) {
    const uint64_t id = NextZipId();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    // The toolbar and the context menu have no progress surface of their own, so
    // a long archive shows up in the operation window like a copy does. The
    // staging shelf draws its own ring and stays out of that window.
    const uint64_t task_id = progress ? 0 : s.ops.CreateExternalTask(Leaf(zip_path), l10n::Get(l10n::StringId::ToolbarToolZip));
    s.zipJobs.push_back({id, zip_path, cancel, task_id});
    // Select the archive once it is listed, the way a paste selects its items.
    if (app::Tab* tab = ActiveTab(s);
        tab && _wcsicmp(fs::NormalizePath(tab->current_path).c_str(), fs::NormalizePath(dir).c_str()) == 0) {
        ArmArrivalSelection(s, nullptr, id, 0);
        ExpectArrival(s, zip_path);
    }
    const HWND hwnd = s.hwnd;
    std::thread([hwnd, id, task_id, paths = std::move(paths), zip_path, options, cancel, progress] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const auto started = std::chrono::steady_clock::now();
        uint64_t last_bytes = 0;
        double peak = 0.0;
        auto last_at = started;
        auto last_post = started;
        auto done = std::make_unique<ZipDone>();
        done->id = id;
        done->task_id = task_id;
        done->zip_path = zip_path;
        done->result = app::CompressToArchive(paths, zip_path, options, cancel.get(),
                                               [&](uint64_t now, uint64_t total, uint64_t items,
                                                   uint64_t total_items) {
            if (progress) {
                if (total > 0)
                    progress->store(static_cast<float>(static_cast<double>(now) / static_cast<double>(total)));
                return;
            }
            if (task_id == 0 || cancel->load()) return;
            const auto clock = std::chrono::steady_clock::now();
            // Four updates a second keep the bar and the speed readable without
            // flooding the window with messages on a fast disk. The final update
            // (everything processed) always goes out.
            const bool finished = now == total && items == total_items;
            if (!finished && clock - last_post < std::chrono::milliseconds(150)) return;
            auto update = std::make_unique<ZipProgress>();
            update->task_id = task_id;
            update->done = now;
            update->total = total;
            update->items_done = items;
            update->items_total = total_items;
            const double seconds = std::chrono::duration<double>(clock - last_at).count();
            if (seconds > 0.05 && now > last_bytes)
                update->bytes_per_second = static_cast<double>(now - last_bytes) / seconds;
            peak = (std::max)(peak, update->bytes_per_second);
            update->peak_bytes_per_second = peak;
            last_bytes = now;
            last_at = clock;
            last_post = clock;
            if (PostMessageW(hwnd, WM_ZIP_PROGRESS, 0, reinterpret_cast<LPARAM>(update.get())))
                update.release();
        });
        if (progress) progress->store(1.0f);
        if (PostMessageW(hwnd, WM_ZIP_COMPRESS, 1, reinterpret_cast<LPARAM>(done.get()))) done.release();
    }).detach();
    return id;
}
} // namespace

namespace {
// The user's format/effort, with the format falling back to ZIP when this build's
// system library cannot write it.
app::ArchiveOptions CurrentArchiveOptions(AppState& s) {
    app::ArchiveOptions options;
    options.format = static_cast<app::ArchiveFormat>(std::clamp(s.appPrefs.zip_format, 0, 2));
    if (!app::ArchiveFormatAvailable(options.format)) options.format = app::ArchiveFormat::Zip;
    options.level = std::clamp(s.appPrefs.zip_level, 0, 3);
    return options;
}
} // namespace

void CompressSelection(AppState& s) {
    std::vector<std::wstring> paths;
    std::wstring dir;
    if (!CompressTarget(s, &paths, &dir)) return;
    const app::ArchiveOptions options = CurrentArchiveOptions(s);
    const std::wstring archive_path = app::ArchivePathFor(paths, dir, options.format);
    StartZip(s, std::move(paths), dir, archive_path, options, nullptr);
}

uint64_t CompressPathsTo(AppState& s, std::vector<std::wstring> paths, const std::wstring& dir,
                         const std::wstring& base_name, std::shared_ptr<std::atomic<float>> progress) {
    if (paths.empty() || dir.empty() || fs::IsVirtualPath(dir) || !app::ZipCompressAvailable()) return 0;
    const app::ArchiveOptions options = CurrentArchiveOptions(s);
    const std::wstring extension = app::ArchiveExtension(options.format);
    std::wstring folder = dir;
    if (folder.back() != L'\\') folder += L'\\';
    std::wstring archive_path = folder + base_name + extension;
    for (int n = 2; GetFileAttributesW(archive_path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n)
        archive_path = folder + base_name + L" (" + std::to_wstring(n) + L")" + extension;
    return StartZip(s, std::move(paths), dir, archive_path, options, std::move(progress));
}

void HandleZipMessage(AppState& s, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ZIP_CANCEL) {
        for (auto& job : s.zipJobs) job.cancel->store(true);
        return;
    }
    if (message == WM_ZIP_PROGRESS) {
        std::unique_ptr<ZipProgress> progress(reinterpret_cast<ZipProgress*>(lparam));
        if (!progress || progress->task_id == 0) return;
        ops::OpStatus status;
        status.active = true;
        status.total_bytes = progress->total;
        status.transferred_bytes = progress->done;
        status.percent = progress->total > 0
            ? static_cast<float>(100.0 * static_cast<double>(progress->done) /
                                 static_cast<double>(progress->total))
            : -1.0f;
        status.total_items = progress->items_total;
        status.completed_items = progress->items_done;
        status.bytes_per_second = progress->bytes_per_second;
        status.peak_bytes_per_second = progress->peak_bytes_per_second;
        status.speed_basis = ops::OpSpeedBasis::Bytes;
        if (progress->bytes_per_second > 1.0 && progress->total > progress->done) {
            status.eta_seconds = static_cast<uint64_t>(
                static_cast<double>(progress->total - progress->done) / progress->bytes_per_second);
        }
        s.ops.UpdateExternalTask(progress->task_id, status);
        return;
    }
    if (wparam != 1) return;   // 0 was the old "still working" snack
    std::unique_ptr<ZipDone> done(reinterpret_cast<ZipDone*>(lparam));
    if (!done) return;
    if (done->task_id != 0) {
        ops::OpStatus final;
        final.active = false;
        final.total_bytes = done->result.bytes;
        final.transferred_bytes = done->result.bytes;
        final.percent = done->result.ok ? 100.0f : -1.0f;
        final.last_error = done->result.error;
        const ops::ExternalOutcome outcome = done->result.ok ? ops::ExternalOutcome::Completed
            : done->result.cancelled ? ops::ExternalOutcome::Cancelled
                                     : ops::ExternalOutcome::Failed;
        s.ops.FinishExternalTask(done->task_id, outcome, final);
    }
    s.zipJobs.erase(std::remove_if(s.zipJobs.begin(), s.zipJobs.end(),
                                   [&](const AppState::ZipJob& j) { return j.id == done->id; }),
                    s.zipJobs.end());
    if (s.trayZip.id == done->id) s.trayZip = {};
    ops::FinishedTask finished;
    finished.task_id = done->id;
    finished.success = done->result.ok;
    NoteArrivalFinished(s, finished);
    if (done->result.ok) {
        const std::wstring parent = fs::ParentPath(done->zip_path);
        s.store.MarkDirty(parent);
        RefreshPath(s, parent, RefreshReason::OperationCompleted);
    }
    // A long run already reported its outcome in the operation window; a snack
    // on top of it would be the same news twice. Short runs never open that
    // window and still get the snack.
    if (!(s.operationWindow && s.operationWindow->IsVisible())) ShowResult(s, *done);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void CancelZipJobs(AppState& s) {
    for (auto& job : s.zipJobs) job.cancel->store(true);
}
}
