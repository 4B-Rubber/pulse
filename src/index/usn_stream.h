#pragma once
#include <windows.h>
#include <winioctl.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

namespace pulse::index {
// The collector owns its handle/cursor. Applying records, merging the index
// and writing cache files never hold its queue lock or delay the next read.
class UsnStream {
public:
    UsnStream(wchar_t letter, uint64_t journal, int64_t cursor, HANDLE signal)
        : signal_(signal), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          thread_([this, letter, journal, cursor] { Read(letter, journal, cursor); }) {}
    ~UsnStream() { SetEvent(stop_); if (thread_.joinable()) thread_.join(); CloseHandle(stop_); }
    bool Take(std::vector<BYTE>& records, int64_t& cursor) {
        std::lock_guard lock(mu_);
        if (error_) return false;
        while (!packets_.empty() && records.size() < 1024 * 1024) {
            auto& packet = packets_.front();
            cursor = *reinterpret_cast<const int64_t*>(packet.data());
            records.insert(records.end(), packet.begin() + sizeof(int64_t), packet.end());
            bytes_ -= packet.size(); packets_.pop_front();
        }
        if (!packets_.empty()) SetEvent(signal_);
        return true;
    }
    DWORD Error() const { return error_.load(); }
private:
    void Failed(DWORD error) { { std::lock_guard lock(mu_); error_ = error; } SetEvent(signal_); }
    void Read(wchar_t letter, uint64_t journal, int64_t cursor) {
        const auto path = L"\\\\.\\" + std::wstring(1, letter) + L":";
        HANDLE volume = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (volume == INVALID_HANDLE_VALUE) { Failed(GetLastError()); return; }
        HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        READ_USN_JOURNAL_DATA_V0 request{};
        request.StartUsn = cursor; request.UsnJournalID = journal;
        request.ReasonMask = 0xffffffffu; request.BytesToWaitFor = 1;
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            std::vector<BYTE> packet(256 * 1024);
            OVERLAPPED operation{}; operation.hEvent = completed; ResetEvent(completed);
            DWORD received = 0;
            bool ok = DeviceIoControl(volume, FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                packet.data(), static_cast<DWORD>(packet.size()), &received, &operation) != FALSE;
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                HANDLE events[]{stop_, completed};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    CancelIoEx(volume, &operation); GetOverlappedResult(volume, &operation, &received, TRUE); break;
                }
                ok = GetOverlappedResult(volume, &operation, &received, FALSE) != FALSE;
            }
            if (!ok) { Failed(GetLastError()); break; }
            if (received < sizeof(int64_t)) { Failed(ERROR_INVALID_DATA); break; }
            packet.resize(received); request.StartUsn = *reinterpret_cast<const int64_t*>(packet.data());
            {
                std::lock_guard lock(mu_);
                if (bytes_ + packet.size() > 64ull * 1024 * 1024) { error_ = ERROR_BUFFER_OVERFLOW; }
                else { bytes_ += packet.size(); packets_.push_back(std::move(packet)); }
            }
            SetEvent(signal_);
            if (error_) break;
        }
        CloseHandle(completed); CloseHandle(volume);
    }
    HANDLE signal_, stop_;
    std::mutex mu_;
    std::deque<std::vector<BYTE>> packets_;
    size_t bytes_ = 0;
    std::atomic<DWORD> error_{0};
    std::thread thread_;
};
}
