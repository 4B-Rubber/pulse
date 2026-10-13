// fs_watch.h — ReadDirectoryChangesW directory watcher.
// Overlapped reads return ERROR_IO_PENDING until a change arrives; that is
// success, not a reason to reopen the directory. CreateFile runs on the
// watch thread so UNC/SMB cannot stall the UI.
#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::fs {
struct DirNotifyEvent;
// Overflow supersedes all pending details for this path. Keep one queue item
// while a UI consumer is paused, instead of accumulating repeated refreshes.
template<class Queue>
void QueueDirectoryNotification(Queue& queue, const std::wstring& path, bool overflow,
                                std::vector<DirNotifyEvent> events);


struct DirNotifyEvent {
    DWORD action = 0;
    std::wstring name;
    std::wstring old_name;
};
template<class Queue>
void QueueDirectoryNotification(Queue& queue, const std::wstring& path, bool overflow,
                                std::vector<DirNotifyEvent> events) {
    for (auto it = queue.begin(); it != queue.end();) {
        if (it->path != path) { ++it; continue; }
        if (!overflow && it->overflow) return;
        if (overflow) it = queue.erase(it);
        else ++it;
    }
    queue.push_back({path, overflow, std::move(events)});
}


class DirWatch {
public:
    using ChangeCallback = std::function<void(bool overflow, std::vector<DirNotifyEvent> events)>;

    DirWatch();
    ~DirWatch();

    bool Start(const std::wstring& path, ChangeCallback cb, bool subtree = false);
    void Stop();
    bool Armed() const;
    const std::wstring& path() const { return path_; }

private:
    struct State;
    std::shared_ptr<State> state_;
    std::wstring path_;
    std::thread thread_;

};

class DirWatchSet {
public:
    using Callback = std::function<void(const std::wstring& path, bool overflow,
                                        std::vector<DirNotifyEvent> events)>;

    void Sync(const std::vector<std::wstring>& paths, Callback cb);
    void Stop();
    bool Armed(const std::wstring& path) const;

private:
    std::mutex mutex_;
    Callback callback_;
    std::unordered_map<std::wstring, std::unique_ptr<DirWatch>> watches_;
};

} // namespace pulse::fs
