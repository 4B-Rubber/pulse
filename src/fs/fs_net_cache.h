// fs_net_cache.h — Persistent UNC directory snapshots + one-shot connectivity probe.
#pragma once
#include "fs_enum.h"
#include "fs_snapshot.h"
#include <cstdint>
#include <string>
#include <windows.h>

namespace pulse::fs {

using UncProbeId = uint64_t;

struct UncProbeResult {
    UncProbeId probe_id = 0;
    std::wstring unc;
    NetStatus status = NetStatus::Unknown;
    DWORD rtt_ms = 0;
    bool completed = true;
    bool final = true; // Timeout is provisional; completed I/O is final.
};

struct NetSnapshotWriteState;
class NetSnapshotWrite {
public:
    NetSnapshotWrite() = default;
    uint64_t Generation() const { return generation_; }
    friend bool SaveNetSnapshot(const std::wstring&, const SnapshotPtr&, const NetSnapshotWrite&);
private:
    std::shared_ptr<NetSnapshotWriteState> state_;
    uint64_t generation_ = 0;
    friend NetSnapshotWrite BeginNetSnapshotWrite(const std::wstring& path);
    friend bool SaveNetSnapshot(const NetSnapshotWrite& request, const SnapshotPtr& snapshot);
};

// Register before enumeration, without disk I/O. All sorts of a normalized
// path share a publication order; only its newest request may commit.
NetSnapshotWrite BeginNetSnapshotWrite(const std::wstring& path);
bool SaveNetSnapshot(const NetSnapshotWrite& request, const SnapshotPtr& snapshot);
using NetSnapshotWriteTicket = NetSnapshotWrite;
bool SaveNetSnapshot(const std::wstring& path, const SnapshotPtr& snapshot, const NetSnapshotWrite& request);
SnapshotPtr LoadNetSnapshot(const std::wstring& path, uint64_t* unix_sec = nullptr);
std::wstring FormatCacheAge(uint64_t unix_sec);

// Background probe: posts `msg` to `hwnd` with lParam = heap UncProbeResult*.
// If the initial timeout result is followed by a final completion, both carry
// the same probe ID.
bool StartUncProbe(HWND hwnd, UINT msg, std::wstring unc, UncProbeId probe_id);

} // namespace pulse::fs
