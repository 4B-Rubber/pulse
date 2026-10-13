// fs_net_cache.cpp — Disk snapshots for UNC folders and a timed connectivity probe.
#include "fs_net_cache.h"
#include "../common/localization.h"
#include <shlobj.h>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace pulse::fs {
#if defined(PULSE_TEST_NET_CACHE)
std::wstring NetCacheDirectoryForTest();
BOOL ProbeAttributesForTest(const std::wstring& path);
DWORD ProbeTimeoutForTest();
void BeforeNetSnapshotCommitForTest(uint64_t generation);
#endif

struct NetSnapshotWriteState {
    std::wstring path;
    std::atomic<uint64_t> latest{0};
    std::mutex commit_mutex;
    uint64_t committed = 0;
};

namespace {

std::mutex write_registry_mutex;
std::unordered_map<std::wstring, std::weak_ptr<NetSnapshotWriteState>> write_registry;

struct SnapshotTempFile {
    std::wstring path;
    FILE* stream = nullptr;
    ~SnapshotTempFile() {
        if (stream) fclose(stream);
        if (!path.empty()) DeleteFileW(path.c_str());
    }
    bool Open(const std::wstring& destination) {
        static std::atomic<uint64_t> serial{0};
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::wstring candidate = destination + L"." + std::to_wstring(GetCurrentProcessId()) +
                L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++serial) + L".tmp";
            HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) continue;
                return false;
            }
            path = candidate;
            const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_WRONLY);
            if (descriptor < 0) { CloseHandle(handle); return false; }
            stream = _fdopen(descriptor, "wb");
            if (!stream) { _close(descriptor); return false; }
            return true;
        }
        return false;
    }
};

std::wstring CacheDir() {
#if defined(PULSE_TEST_NET_CACHE)
    return NetCacheDirectoryForTest();
#endif
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return L"";
    std::wstring dir = std::wstring(path) + L"\\Pulse";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\netcache";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

uint64_t HashPath(const std::wstring& p) {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t c : p) {
        h ^= static_cast<uint16_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring CacheFile(const std::wstring& path) {
    const std::wstring normalized = NormalizePath(path);
    if (normalized.empty()) return {};
    std::wstring dir = CacheDir();
    if (dir.empty()) return L"";
    wchar_t name[32];
    swprintf_s(name, L"%016llX.bin", HashPath(normalized));
    return dir + L"\\" + name;
}

uint64_t NowUnix() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

NetSnapshotWrite BeginNetSnapshotWrite(const std::wstring& path) {
    NetSnapshotWrite request;
    if (!IsUncPath(path)) return request;
    const auto normalized = NormalizePath(path);
    if (normalized.empty()) return request;
    std::lock_guard lock(write_registry_mutex);
    // Retain ordering while any queued/running request owns the state, without
    // retaining every UNC directory ever visited for the process lifetime.
    for (auto it = write_registry.begin(); it != write_registry.end(); ) {
        if (it->second.expired()) it = write_registry.erase(it);
        else ++it;
    }
    auto& weak = write_registry[normalized];
    request.state_ = weak.lock();
    if (!request.state_) {
        request.state_ = std::make_shared<NetSnapshotWriteState>();
        request.state_->path = normalized;
        weak = request.state_;
    }
    request.generation_ = ++request.state_->latest;
    return request;
}

bool SaveNetSnapshot(const NetSnapshotWrite& request, const SnapshotPtr& snapshot) {
    const auto& state = request.state_;
    if (!snapshot || !state || state->latest.load() != request.generation_ || snapshot->size() > 500000) return false;
    for (const auto& entry : *snapshot) if (entry.name.size() > 1024) return false;
    const std::wstring file = CacheFile(state->path);
    if (file.empty()) return false;
    SnapshotTempFile tmp;
    if (!tmp.Open(file)) return false;
    bool complete = true;
    const auto write = [&](const void* data, size_t bytes) {
        if (complete && fwrite(data, 1, bytes, tmp.stream) != bytes) complete = false;
    };
    write("PNCH", 4);
    uint32_t ver = 2;
    uint64_t ts = NowUnix();
    uint32_t count = static_cast<uint32_t>(snapshot->size());
    write(&ver, 4);
    write(&ts, 8);
    write(&count, 4);
    for (const auto& e : *snapshot) {
        uint8_t flags = (e.is_dir ? 1 : 0) | (e.is_reparse ? 2 : 0);
        uint64_t mtime = (static_cast<uint64_t>(e.mtime.dwHighDateTime) << 32) | e.mtime.dwLowDateTime;
        uint32_t nlen = static_cast<uint32_t>(e.name.size());
        write(&flags, 1);
        write(&e.attrs, 4);
        write(&e.reparse_tag, 4);
        write(&e.size, 8);
        write(&mtime, 8);
        write(&nlen, 4);
        write(e.name.data(), nlen * sizeof(wchar_t));
        if (!complete) return false;
    }
    if (fflush(tmp.stream) != 0 || !FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(tmp.stream))))) return false;
    if (fclose(std::exchange(tmp.stream, nullptr)) != 0 || !complete) return false;
    // Only background publishers take this lock. Request registration never
    // waits on disk I/O. A newer publisher cannot be overtaken by an old one.
#if defined(PULSE_TEST_NET_CACHE)
    BeforeNetSnapshotCommitForTest(request.generation_);
#endif
    std::lock_guard lock(state->commit_mutex);
    if (state->latest.load() != request.generation_ || state->committed >= request.generation_) return false;
    if (!MoveFileExW(tmp.path.c_str(), file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    state->committed = request.generation_;
    return true;
}

SnapshotPtr LoadNetSnapshot(const std::wstring& path, uint64_t* unix_sec) {
    if (!IsUncPath(path)) return nullptr;
    const std::wstring file = CacheFile(path);
    if (file.empty()) return nullptr;
    std::ifstream f(file, std::ios::binary);
    if (!f) return nullptr;
    char magic[4]{};
    f.read(magic, 4);
    if (std::string(magic, 4) != "PNCH") return nullptr;
    uint32_t ver = 0, count = 0;
    uint64_t ts = 0;
    f.read(reinterpret_cast<char*>(&ver), 4);
    f.read(reinterpret_cast<char*>(&ts), 8);
    f.read(reinterpret_cast<char*>(&count), 4);
    if ((ver != 1 && ver != 2) || count > 500000) return nullptr;
    auto entries = std::make_shared<std::vector<DirEntry>>();
    entries->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t flags = 0;
        uint32_t attrs = 0, nlen = 0, reparse_tag = 0;
        uint64_t size = 0, mtime = 0;
        f.read(reinterpret_cast<char*>(&flags), 1);
        f.read(reinterpret_cast<char*>(&attrs), 4);
        if (ver >= 2) f.read(reinterpret_cast<char*>(&reparse_tag), 4);
        f.read(reinterpret_cast<char*>(&size), 8);
        f.read(reinterpret_cast<char*>(&mtime), 8);
        f.read(reinterpret_cast<char*>(&nlen), 4);
        if (!f || nlen > 1024) return nullptr;
        DirEntry e;
        e.name.assign(nlen, L'\0');
        f.read(reinterpret_cast<char*>(e.name.data()), nlen * sizeof(wchar_t));
        if (!f) return nullptr;
        e.attrs = attrs;
        e.reparse_tag = reparse_tag;
        e.size = size;
        e.mtime.dwLowDateTime = static_cast<DWORD>(mtime);
        e.mtime.dwHighDateTime = static_cast<DWORD>(mtime >> 32);
        e.is_dir = (flags & 1) != 0;
        e.is_reparse = (flags & 2) != 0;
        entries->push_back(std::move(e));
    }
    if (unix_sec) *unix_sec = ts;
    return entries;
}

std::wstring FormatCacheAge(uint64_t unix_sec) {
    if (unix_sec == 0) return l10n::Pick(L"刚才", L"Just now");
    const uint64_t now = NowUnix();
    if (now <= unix_sec) return l10n::Pick(L"刚才", L"Just now");
    const uint64_t sec = now - unix_sec;
    if (sec < 60) return std::to_wstring(sec) + l10n::Pick(L" 秒前", L" sec ago");
    if (sec < 3600) return std::to_wstring(sec / 60) + l10n::Pick(L" 分钟前", L" min ago");
    if (sec < 86400) return std::to_wstring(sec / 3600) + l10n::Pick(L" 小时前", L" hr ago");
    const uint64_t days = sec / 86400;
    return std::to_wstring(days) + l10n::Pick(L" 天前", days == 1 ? L" day ago" : L" days ago");
}

namespace {

std::atomic<unsigned> active_probes{0};
constexpr unsigned kMaxActiveProbes = 8;
struct ProbeJob {
    std::wstring unc;
    UncProbeId probe_id = 0;
    HWND hwnd = nullptr;
    UINT msg = 0;
    HANDLE done = nullptr;
    HANDLE thread = nullptr;
    BOOL ok = FALSE;
    DWORD rtt_ms = 0;
    bool owns_permit = true;
    void ReleasePermit() {
        if (owns_permit) { --active_probes; owns_permit = false; }
    }
    ~ProbeJob() {
        if (thread) {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        if (done) CloseHandle(done);
        ReleasePermit();
    }
};

DWORD WINAPI ProbeInner(LPVOID param) {
    auto* j = static_cast<ProbeJob*>(param);
    const ULONGLONG t0 = GetTickCount64();
#if defined(PULSE_TEST_NET_CACHE)
    j->ok = ProbeAttributesForTest(j->unc);
#else
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    j->ok = GetFileAttributesExW(j->unc.c_str(), GetFileExInfoStandard, &fad);
#endif
    j->rtt_ms = static_cast<DWORD>(GetTickCount64() - t0);
    SetEvent(j->done);
    return 0;
}

void PostProbe(const ProbeJob& job, bool completed, DWORD elapsed) noexcept {
    try {
        auto result = std::make_unique<UncProbeResult>();
        result->probe_id = job.probe_id;
        result->unc = job.unc;
        result->completed = completed;
        result->final = completed;
        // The timeout path MUST NOT inspect worker-owned fields: only a successful
        // event/thread wait publishes ok/rtt_ms to this thread.
        result->rtt_ms = completed ? job.rtt_ms : elapsed;
        result->status = !completed || !job.ok ? NetStatus::Offline :
            (job.rtt_ms > 800 ? NetStatus::Slow : NetStatus::Online);
        if (PostMessageW(job.hwnd, job.msg, 0, reinterpret_cast<LPARAM>(result.get())))
            result.release();
    } catch (...) {
        // Allocation failure must not tear down a job still owned by its I/O thread.
    }
}

} // namespace

#if defined(PULSE_TEST_NET_CACHE)
unsigned ActiveUncProbesForTest() { return active_probes.load(); }
#endif

bool StartUncProbe(HWND hwnd, UINT msg, std::wstring unc, UncProbeId probe_id) {
    if (!hwnd || unc.empty() || probe_id == 0) return false;
    if (active_probes.fetch_add(1) >= kMaxActiveProbes) { --active_probes; return false; }
    std::unique_ptr<ProbeJob> job;
    try { job = std::make_unique<ProbeJob>(); }
    catch (...) { --active_probes; return false; }
    job->unc = std::move(unc);
    job->probe_id = probe_id;
    job->hwnd = hwnd;
    job->msg = msg;
    job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->done) return false;
    try {
        std::thread([job = std::move(job)] {
            job->thread = CreateThread(nullptr, 0, ProbeInner, job.get(), 0, nullptr);
            if (!job->thread) { job->ReleasePermit(); PostProbe(*job, true, 0); return; }
#if defined(PULSE_TEST_NET_CACHE)
            const DWORD timeout = ProbeTimeoutForTest();
#else
            constexpr DWORD timeout = 1500;
#endif
            const ULONGLONG started = GetTickCount64();
            const bool completed = WaitForSingleObject(job->done, timeout) == WAIT_OBJECT_0;
            if (!completed) PostProbe(*job, false, static_cast<DWORD>(GetTickCount64() - started));
            WaitForSingleObject(job->thread, INFINITE);
            // Capacity must be reusable before the final message wakes the UI.
            job->ReleasePermit();
            PostProbe(*job, true, 0);
        }).detach();
    } catch (...) { return false; }
    return true;
}

} // namespace pulse::fs

namespace pulse::fs {
bool SaveNetSnapshot(const std::wstring& path, const SnapshotPtr& snapshot, const NetSnapshotWrite& request) {
    return request.state_ && request.state_->path == NormalizePath(path) && SaveNetSnapshot(request, snapshot);
}
}
