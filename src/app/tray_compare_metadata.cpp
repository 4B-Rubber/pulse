#include "tray_compare_metadata.h"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace pulse::app {
namespace {
std::atomic<unsigned> active_workers{0};
constexpr unsigned kWorkerLimit = 4;
}
struct TrayCompareMetadata::State {
    struct Job {
        TrayMetadataKey key;
        uint64_t generation = 0;
        std::atomic<bool> cancelled{false};
    };
    std::mutex mutex;
    std::condition_variable wake;
    Query query;
    std::shared_ptr<Job> current, pending;
    TrayMetadataSnapshot result;
    uint64_t generation = 0, retry_at = 0;
    HWND notify = nullptr;
    HANDLE thread = nullptr;
    bool started = false, stopped = false;
    ~State() { if (thread) CloseHandle(thread); }
    static void Run(const std::shared_ptr<State>& state) {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [&] { return state->stopped || state->pending || !state->current; });
                if (state->stopped) return;
                if (!state->current) {
                    state->started = false;
                    if (state->thread) CloseHandle(state->thread);
                    state->thread = nullptr;
                    return;
                }
                job = std::move(state->pending);
            }
            TrayMetadataSnapshot result;
            result.generation = job->generation;
            for (size_t i = 0; i < 2 && !job->cancelled; ++i) {
                try { result.error = state->query(job->key.paths[i], result.files[i], job->cancelled); }
                catch (...) { result.error = ERROR_UNHANDLED_EXCEPTION; }
                if (result.error) break;
                if (result.files[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { result.error = ERROR_DIRECTORY; break; }
            }
            HWND notify = nullptr;
            {
                std::lock_guard lock(state->mutex);
                if (state->stopped || job->cancelled || state->current != job) continue;
                result.status = result.error ? TrayMetadataSnapshot::Status::Error : TrayMetadataSnapshot::Status::Ready;
                state->result = result;
                state->retry_at = GetTickCount64() + (result.error ? 5000 : 2000);
                notify = state->notify;
            }
            if (notify) InvalidateRect(notify, nullptr, FALSE);
        }
    }
};
TrayCompareMetadata::TrayCompareMetadata(Query query) : state_(std::make_shared<State>()) {
    state_->query = query ? std::move(query) : Query([](const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& data, const std::atomic<bool>& cancelled) {
        if (cancelled) return DWORD(ERROR_CANCELLED);
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            const DWORD error = GetLastError();
            return error ? error : DWORD(ERROR_GEN_FAILURE);
        }
        return cancelled ? DWORD(ERROR_CANCELLED) : DWORD(ERROR_SUCCESS);
    });
}
TrayCompareMetadata::~TrayCompareMetadata() {
    const auto state = state_;
    std::lock_guard lock(state->mutex);
    state->stopped = true;
    if (state->current) state->current->cancelled = true;
    state->pending.reset(); state->notify = nullptr;
    if (state->thread) CancelSynchronousIo(state->thread);
    state->wake.notify_one();
}
void TrayCompareMetadata::Cancel() {
    const auto state = state_;
    std::lock_guard lock(state->mutex);
    if (!state->current) return;
    state->current->cancelled = true;
    state->current.reset(); state->pending.reset(); state->notify = nullptr;
    if (state->thread) CancelSynchronousIo(state->thread);
    state->wake.notify_one();
}
TrayMetadataSnapshot TrayCompareMetadata::Read(const TrayMetadataKey& key, HWND notify) {
    const auto state = state_;
    std::lock_guard lock(state->mutex);
    state->notify = notify;
    if (state->current && state->current->key == key &&
        (state->result.status == TrayMetadataSnapshot::Status::Pending || GetTickCount64() < state->retry_at)) return state->result;
    if (state->current) state->current->cancelled = true;
    if (state->thread) CancelSynchronousIo(state->thread);
    auto job = std::make_shared<State::Job>(); job->key = key; job->generation = ++state->generation;
    state->current = state->pending = job;
    state->result = {}; state->result.generation = job->generation;
    if (!state->started) {
        // Retired non-cooperating providers count too: repeated window closure
        // cannot accumulate an unbounded number of detached metadata threads.
        if (active_workers.fetch_add(1) >= kWorkerLimit) {
            --active_workers;
            state->pending.reset(); state->result.status = TrayMetadataSnapshot::Status::Error;
            state->result.error = ERROR_BUSY; state->retry_at = GetTickCount64() + 5000;
            return state->result;
        }
        try {
            std::thread worker([state] {
                struct Release { ~Release() { --active_workers; } } release;
                State::Run(state);
            });
            DuplicateHandle(GetCurrentProcess(), worker.native_handle(), GetCurrentProcess(), &state->thread,
                THREAD_TERMINATE, FALSE, 0);
            worker.detach(); state->started = true;
        } catch (...) {
            --active_workers;
            state->pending.reset(); state->result.status = TrayMetadataSnapshot::Status::Error;
            state->result.error = ERROR_NOT_ENOUGH_MEMORY; state->retry_at = GetTickCount64() + 5000;
        }
    }
    state->wake.notify_one();
    return state->result;
}
}
