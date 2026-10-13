#include "../index/content_instant_session.h"
#include "../index/content_scope.h"
#include "../index/content_search_protocol.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <thread>

int wmain() {
    using namespace pulse::index;
    namespace fs = std::filesystem;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto base = parent / (L"content-reconcile-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(base)) return 1;
    const auto root = base / L"files", imported = root / L"imported";
    fs::create_directories(imported);
    fs::create_directory(base / L"profile");
    const auto existing = root / L"existing.txt", fresh = root / L"fresh.txt", child = imported / L"child.txt";
    std::ofstream(existing) << "old";
    std::ofstream(fresh) << "new";
    std::ofstream(child) << "child";
    SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", (base / L"logs").c_str());
    {
        ContentIndex index((base / L"profile" / L"content.sqlite").wstring(), ContentAgentMode::Instant);
        ContentIndexConfig config; config.roots = {{root.wstring()}};
        check(index.Configure(config), "isolated instant index configured");
        for (int scenario = 0; scenario < 10; ++scenario) {
            printf("[INFO] scenario=%d\n", scenario);
            ContentSearchRequest request;
            request.generation = 7800 + scenario; request.root = root.wstring();
            request.needle = L"marker"; request.subscribe = request.task_scan = true;
            const bool is_import = scenario == 3 || scenario == 4 || scenario == 7 || scenario == 8;
            const auto changed = scenario == 1 ? fresh : is_import ? imported :
                scenario == 6 ? root / L"gone.txt" : existing;
            std::atomic<bool> cancelled{false};
            std::atomic<bool> probe_failed{false};
            size_t metadata_attempts = 0, listing_attempts = 0, full_scans = 0, local_scans = 0, pauses = 0;
            bool got_new = false, got_removed = false, retained_initial = false;
            ContentSearchProgress paused;
            ContentInstantSessionHooks hooks;
            ContentInstantSessionHooks::Enqueue enqueue;
            hooks.arm = [&](auto notify, auto) { enqueue = std::move(notify); };
            hooks.attributes = [&](const std::wstring& path) {
                if (path == changed.wstring() || (scenario == 4 && path == child.wstring())) {
                    ++metadata_attempts;
                    const bool deny = scenario == 2 || scenario == 5 || scenario == 9 ||
                        ((scenario == 0 || scenario == 1) && metadata_attempts == 1) ||
                        (scenario == 4 && path == child.wstring() && metadata_attempts == 2);
                    if (deny) {
                        probe_failed = true;
                        if (scenario == 5) cancelled = true;
                        SetLastError(ERROR_ACCESS_DENIED); return DWORD{INVALID_FILE_ATTRIBUTES};
                    }
                }
                return GetFileAttributesW(path.c_str());
            };
            hooks.list_directory = [&](const std::wstring& path, std::vector<std::wstring>& children) {
                ++listing_attempts;
                // A partial listing must not erase the failed event or publish its prefix.
                if ((scenario == 3 && listing_attempts == 1) || scenario == 7) {
                    children.push_back(child.wstring()); return DWORD{ERROR_NETNAME_DELETED};
                }
                std::error_code error;
                fs::directory_iterator it(path, error), end;
                for (; !error && it != end; it.increment(error)) children.push_back(it->path().wstring());
                return static_cast<DWORD>(error.value());
            };
            if (scenario == 8) hooks.list_directory = {};
            hooks.search = [&](const auto& query, const auto&, auto deliver) {
                ContentSearchProgress progress; progress.generation = query.generation; progress.done = true;
                ContentHit hit;
                if (query.candidate_paths.empty()) {
                    ++full_scans;
                    hit.path = scenario == 6 ? changed.wstring() : existing.wstring();
                    hit.name = L"initial"; hit.snippet = L"old";
                } else {
                    ++local_scans; hit.path = query.candidate_paths.front(); hit.name = L"updated"; hit.snippet = L"new";
                }
                const bool accepted = deliver(progress, {hit});
                if (query.candidate_paths.empty()) enqueue(changed.wstring(), is_import);
                return accepted;
            };
            std::jthread deadline([&](std::stop_token stopped) {
                for (int tick = 0; tick < 400 && !stopped.stop_requested(); ++tick) {
                    if (scenario == 9 && probe_failed) { Sleep(40); cancelled = true; return; }
                    Sleep(10);
                }
                if (!stopped.stop_requested()) cancelled = true;
            });
            RunInstantContentSession(index, request, cancelled, [&](const auto& progress, auto hits) {
                if (!progress.delta) retained_initial = !hits.empty();
                for (const auto& hit : hits) {
                    if (progress.delta && hit.removed) got_removed = true;
                    if (progress.delta && hit.snippet == L"new") got_new = true;
                }
                if (progress.subscription_error) { ++pauses; paused = progress; }
                if ((got_new || got_removed) && !progress.subscription_error) cancelled = true;
                return true;
            }, &hooks);
            deadline.request_stop();
            check(full_scans == 1 && retained_initial, "reconciliation retains initial results without a full rescan");
            if (scenario == 2 || scenario == 7) {
                check(pauses == 1 && !paused.live && paused.done && !got_removed && !got_new && local_scans == 0,
                    "permanent failure pauses coverage without deleting or publishing partial results");
                check((scenario == 2 ? metadata_attempts : listing_attempts) == 3,
                    "permanent failure uses exactly three bounded attempts");
                check(paused.subscription_failure == (scenario == 2 ? ContentSubscriptionFailure::Metadata :
                    ContentSubscriptionFailure::DirectoryEnumeration), "pause reports the failing reconciliation stage");
                pulse::ipc::PayloadWriter writer; content::PutSubscriptionStatus(writer, paused);
                pulse::ipc::PayloadReader reader(writer.data().data(), writer.data().size());
                ContentSearchProgress decoded;
                check(content::GetSubscriptionStatus(reader, decoded) && decoded.subscription_failure == paused.subscription_failure,
                    "new failure source round-trips through the production protocol");
            } else if (scenario == 5 || scenario == 9) {
                check(metadata_attempts == 1 && local_scans == 0 && !got_removed && pauses == 0,
                    "cancellation ends retry promptly and retains prior rows");
            } else if (scenario == 6) {
                check(got_removed && local_scans == 0 && pauses == 0, "known missing path still removes its old result");
            } else {
                check(got_new && local_scans == 1 && pauses == 0 && !got_removed,
                    "one event recovers after transient failure without a second notification");
                check(scenario == 8 ? listing_attempts == 0 : scenario == 3 || scenario == 4 ? listing_attempts == 2 : metadata_attempts == 2,
                    "transient retry revisits the failed path or imported directory");
            }
        }
    }
    if (base.parent_path() != parent || !base.filename().wstring().starts_with(L"content-reconcile-")) return 1;
    std::error_code error; fs::remove_all(base, error);
    check(!error && !fs::exists(base), "owned reconciliation fixture removed");
    return failures ? 1 : 0;
}
