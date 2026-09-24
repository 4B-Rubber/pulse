#include "network_agent_client.h"
#include "../ipc/protocol.h"
#include <algorithm>
#include <chrono>
#include <shellapi.h>

namespace pulse::index {
namespace {

using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

// The status poll runs every second. A handshake that keeps failing must not
// turn into one created process per tick.
constexpr ULONGLONG kSpawnRetryDelayMs = 5000;

std::vector<uint8_t> QueryPayload(const Query& query) {
    PayloadWriter writer;
    uint32_t flags = 0;
    if (query.rank) flags |= 1u;
    if (query.folders_only) flags |= 2u;
    if (query.sort_desc) flags |= 4u;
    writer.PutU32(flags);
    writer.PutU32(static_cast<uint32_t>(query.sort));
    writer.PutU32(static_cast<uint32_t>(query.limit));
    writer.PutU32(static_cast<uint32_t>(query.offset));
    writer.PutString(query.needle);
    writer.PutString(query.path_prefix);
    return writer.data();
}

bool ReadFrame(HANDLE pipe, uint32_t& type, uint32_t& id, std::vector<uint8_t>& payload) {
    MsgHeader header{};
    if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
        header.magic != agent::kMagic || header.payload_size > agent::kMaxPayload) return false;
    payload.resize(header.payload_size);
    if (!payload.empty() && !PipeRead(pipe, payload.data(), header.payload_size)) return false;
    type = header.type;
    id = header.request_id;
    return true;
}

} // namespace

std::wstring NetworkAgentClient::ExePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return L"Pulse.Index.exe";
    return std::wstring(path, slash + 1) + L"Pulse.Index.exe";
}

bool NetworkAgentClient::EnsureAgent(bool force) {
    if (agent_process_ && WaitForSingleObject(agent_process_, 0) == WAIT_TIMEOUT) return true;
    if (agent_process_) {
        CloseHandle(agent_process_);
        agent_process_ = nullptr;
        // An agent that outlived the retry window died of something other than a
        // spawn loop, so the next poll may start a fresh one right away; a process
        // that died in the same breath as its creation keeps the delay.
        if (last_spawn_try_ != 0 && GetTickCount64() - last_spawn_try_ >= kSpawnRetryDelayMs)
            last_spawn_try_ = 0;
    }
    // Another Pulse window (or one that has just exited) may already own the
    // singleton and serve the pipe. A second agent would lose that race and exit
    // at once, so reuse the running instance instead of creating one process per
    // status poll.
    if (HANDLE serving = OpenMutexW(SYNCHRONIZE, FALSE, agent::kAgentSingletonName)) {
        CloseHandle(serving);
        return true;
    }
    // Starting the agent is what paints the shell's "starting" cursor, so it is
    // worth a process only once the user has server folders to index.
    if (!force && !server_folders_configured_.load()) return false;
    const ULONGLONG now = GetTickCount64();
    if (last_spawn_try_ != 0 && now - last_spawn_try_ < kSpawnRetryDelayMs) return false;
    last_spawn_try_ = now;
    if (!EnsureAgentJob()) return false;
    const std::wstring exe = ExePath();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + exe + L"\" --network-agent";
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
        return false;
    // The agent exists to serve this process and must not outlive it. The job
    // kills it when the last handle closes, which also covers a Pulse crash.
    if (!AssignProcessToJobObject(agent_job_, process.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, error);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        SetLastError(error);
        return false;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    agent_process_ = process.hProcess;
    return true;
}

bool NetworkAgentClient::EnsureAgentJob() {
    if (agent_job_) return true;
    agent_job_ = CreateJobObjectW(nullptr, nullptr);
    if (!agent_job_) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(agent_job_, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        CloseHandle(agent_job_);
        agent_job_ = nullptr;
        return false;
    }
    return true;
}

bool NetworkAgentClient::OpenPipe(HANDLE& pipe) {
    pipe = CreateFileW(agent::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return true;
    if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(agent::kPipeName, 1000);
    pipe = CreateFileW(agent::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    return pipe != INVALID_HANDLE_VALUE;
}

bool NetworkAgentClient::Request(uint32_t type, uint32_t id,
                                 const std::vector<uint8_t>& payload,
                                 uint32_t& response_type,
                                 std::vector<uint8_t>& response,
                                 bool force_agent) {
    std::lock_guard<std::mutex> request_lock(request_mu_);
    if (!running_ || !EnsureAgent(force_agent)) return false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    if (!OpenPipe(pipe)) return false;
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (!running_) {
            CloseHandle(pipe);
            return false;
        }
        active_pipe_ = pipe;
    }
    const auto close_pipe = [this, pipe] {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (active_pipe_ != pipe) return;
        active_pipe_ = INVALID_HANDLE_VALUE;
        CloseHandle(pipe);
    };
    const MsgHeader header = agent::MakeHeader(type, id, static_cast<uint32_t>(payload.size()));
    const bool sent = PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) &&
                      (payload.empty() || PipeWrite(pipe, payload.data(), static_cast<DWORD>(payload.size())));
    if (!sent) {
        close_pipe();
        return false;
    }
    uint32_t response_id = 0;
    const bool received = ReadFrame(pipe, response_type, response_id, response) && response_id == id;
    close_pipe();
    return received;
}

void NetworkAgentClient::Start(HWND notify, UINT status_msg, UINT search_msg) {
    Stop();
    notify_ = notify;
    status_msg_ = status_msg;
    search_msg_ = search_msg;
    running_ = true;
    EnsureAgentJob();
    status_thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(status_mu_);
        while (running_) {
            lock.unlock();
            RefreshRoots();
            lock.lock();
            if (status_cv_.wait_for(lock, std::chrono::seconds(1),
                                    [this] { return !running_; })) break;
        }
    });
    search_thread_ = std::thread([this] { SearchLoop(); });
}

void NetworkAgentClient::Stop() {
    running_ = false;
    status_cv_.notify_all();
    search_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (active_pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(active_pipe_);
            active_pipe_ = INVALID_HANDLE_VALUE;
        }
    }
    if (status_thread_.joinable()) CancelSynchronousIo(status_thread_.native_handle());
    if (search_thread_.joinable()) CancelSynchronousIo(search_thread_.native_handle());
    if (status_thread_.joinable()) status_thread_.join();
    if (search_thread_.joinable()) search_thread_.join();
    std::lock_guard<std::mutex> lock(request_mu_);
    if (agent_process_) {
        CloseHandle(agent_process_);
        agent_process_ = nullptr;
    }
    // Ending the job ends an agent this client started, so an exited Pulse never
    // leaves the per-user agent behind. An agent owned by another window is not
    // in this job and keeps running for that window.
    if (agent_job_) {
        CloseHandle(agent_job_);
        agent_job_ = nullptr;
    }
}

void NetworkAgentClient::SearchAsync(const Query& query, uint32_t id) {
    if (!running_) return;
    latest_search_id_.store(id);
    {
        std::lock_guard<std::mutex> lock(search_mu_);
        pending_query_ = query;
        pending_search_id_ = id;
        pending_searches_[query.session_id]={id,query};session_requests_[query.session_id]=id;
        have_pending_search_ = true;
    }
    search_cv_.notify_one();
}

void NetworkAgentClient::SearchLoop() {
    std::unique_lock<std::mutex> lock(search_mu_);
    while (running_) {
        search_cv_.wait(lock, [this] { return !running_ || have_pending_search_; });
        if (!running_) break;
        auto next=pending_searches_.begin();
        if(next==pending_searches_.end()) {have_pending_search_=false;continue;}
        Query query = std::move(next->second.second);
        const uint32_t id = next->second.first;
        pending_searches_.erase(next);have_pending_search_=!pending_searches_.empty();
        lock.unlock();
        SearchRequest(std::move(query), id);
        lock.lock();
    }
}

void NetworkAgentClient::SearchRequest(Query query, uint32_t id) {
    uint32_t response_type = 0;
    std::vector<uint8_t> payload;
    if (!Request(agent::REQ_SEARCH, id, QueryPayload(query), response_type, payload) ||
        response_type != agent::RSP_SEARCH) return;
    {std::lock_guard lock(search_mu_);auto current=session_requests_.find(query.session_id);if(current==session_requests_.end()||current->second!=id) return;}
    PayloadReader reader(payload.data(), payload.size());
    uint32_t total = 0, count = 0;
    if (!reader.GetU32(total) || !reader.GetU32(count)) return;
    SearchResult result;
    result.total = total;
    result.hits.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Hit hit;
        uint32_t flags = 0, size_lo = 0, size_hi = 0, time_lo = 0, time_hi = 0;
        if (!reader.GetString(hit.path) || !reader.GetString(hit.name) || !reader.GetU32(flags) ||
            !reader.GetU32(size_lo) || !reader.GetU32(size_hi) ||
            !reader.GetU32(time_lo) || !reader.GetU32(time_hi)) return;
        hit.is_dir = (flags & 1u) != 0;
        hit.size = (static_cast<uint64_t>(size_hi) << 32) | size_lo;
        hit.mtime = (static_cast<uint64_t>(time_hi) << 32) | time_lo;
        result.hits.push_back(std::move(hit));
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        results_[id] = std::move(result);
    }
    if (notify_ && search_msg_) PostMessageW(notify_, search_msg_, id, 0);
}

bool NetworkAgentClient::TakeResult(uint32_t id, SearchResult& result) {
    std::lock_guard<std::mutex> lock(mu_);
    auto found=results_.find(id);if(found==results_.end()) return false;
    result=std::move(found->second);results_.erase(found);
    return true;
}

void NetworkAgentClient::RefreshRoots() {
    uint32_t response_type = 0;
    std::vector<uint8_t> payload;
    if (!Request(agent::REQ_ROOTS, 1, {}, response_type, payload) || response_type != agent::RSP_ROOTS)
        return;
    PayloadReader reader(payload.data(), payload.size());
    uint32_t count = 0;
    if (!reader.GetU32(count)) return;
    std::vector<NetworkRootInfo> roots;
    roots.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        NetworkRootInfo root;
        uint32_t flags = 0, progress = 0, lo = 0, hi = 0;
        if (!reader.GetString(root.path) || !reader.GetU32(flags) || !reader.GetU32(progress) ||
            !reader.GetU32(lo) || !reader.GetU32(hi) || !reader.GetString(root.state) ||
            !reader.GetString(root.error)) return;
        root.online = (flags & 1u) != 0;
        root.building = (flags & 2u) != 0;
        root.watching = (flags & 4u) != 0;
        root.progress = progress;
        root.indexed_items = (static_cast<uint64_t>(hi) << 32) | lo;
        roots.push_back(std::move(root));
    }
    // Any window that can see configured roots knows the agent is worth keeping:
    // another window adding the first root, or owning the agent and then exiting,
    // must not leave this session dormant.
    if (!roots.empty()) server_folders_configured_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        roots_ = std::move(roots);
    }
    if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
}

std::vector<NetworkRootInfo> NetworkAgentClient::Roots() const {
    std::lock_guard<std::mutex> lock(mu_);
    return roots_;
}

bool NetworkAgentClient::AddRoot(const std::wstring& path, std::wstring* error) {
    PayloadWriter writer;
    writer.PutString(path);
    uint32_t response_type = 0;
    std::vector<uint8_t> payload;
    // Adding the first server folder is what brings the agent up; nothing is
    // configured yet, so this request has to start it.
    if (!Request(agent::REQ_ADD_ROOT, 2, writer.data(), response_type, payload, true) ||
        response_type != agent::RSP_RESULT) return false;
    PayloadReader reader(payload.data(), payload.size());
    uint32_t ok = 0;
    std::wstring message;
    if (!reader.GetU32(ok) || !reader.GetString(message)) return false;
    if (!ok && error) *error = std::move(message);
    // This session now has a server folder; keep the agent alive for the rest of
    // the run even before the next launch reads the config.
    if (ok) server_folders_configured_ = true;
    RefreshRoots();
    return ok != 0;
}

bool NetworkAgentClient::RemoveRoot(const std::wstring& path, std::wstring* error) {
    PayloadWriter writer;
    writer.PutString(path);
    uint32_t response_type = 0;
    std::vector<uint8_t> payload;
    if (!Request(agent::REQ_REMOVE_ROOT, 3, writer.data(), response_type, payload) ||
        response_type != agent::RSP_RESULT) return false;
    PayloadReader reader(payload.data(), payload.size());
    uint32_t ok = 0;
    std::wstring message;
    if (!reader.GetU32(ok) || !reader.GetString(message)) return false;
    if (!ok && error) *error = std::move(message);
    RefreshRoots();
    return ok != 0;
}

void NetworkAgentClient::Rebuild(const std::wstring& path) {
    PayloadWriter writer;
    writer.PutString(path);
    uint32_t response_type = 0;
    std::vector<uint8_t> ignored;
    Request(agent::REQ_REBUILD, 4, writer.data(), response_type, ignored);
    RefreshRoots();
}

} // namespace pulse::index
