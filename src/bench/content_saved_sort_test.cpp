#include "../index/content_search_client.h"
#include "../index/content_search_session.h"
#include "../index/content_result_store.h"
#include <cstdio>
namespace pulse::index {
struct ContentRefreshTestPeer {
    static ContentSearchRequest Queue(ContentSearchRequest request, ContentAgentMode mode, bool persistent = true) {
        ContentSearchClient client;
        client.mode_ = mode;
        client.persistent_enabled_ = persistent;
        client.running_ = true;
        client.SearchAsync(std::move(request));
        auto queued = std::move(client.pending_requests_.front());
        client.running_ = false;
        return queued;
    }
};
}
int main() {
    using namespace pulse::index;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    auto row = [](const std::shared_ptr<ContentResultStore>& store, size_t at, ContentResultStore::Row& output) {
        const auto deadline = GetTickCount64() + 5000;
        while (GetTickCount64() < deadline) { if (store->Get(at, output)) return true; Sleep(2); }
        return false;
    };
    ContentSearchRequest saved;
    saved.generation = 100; saved.session_id = 1;
    saved.indexed = saved.paged_results = saved.subscribe = true;
    saved.mode = ContentSearchMode::Content;
    saved.root = L"C:\\fixture"; saved.needle = L"needle";
    for (auto sort : {ContentResultSort::Index, ContentResultSort::Name, ContentResultSort::Size,
                      ContentResultSort::Mtime, ContentResultSort::Path, ContentResultSort::Type}) {
        for (bool descending : {false, true}) {
            saved.sort = sort; saved.sort_desc = descending;
            auto request = ContentRefreshTestPeer::Queue(saved, ContentAgentMode::Instant);
            check(request.task_scan && !request.incremental && !request.after_revision, "instant saved search normalized before session creation");
            ContentSearchSession session(request, nullptr, 0);
            ContentHit z; z.file_id = 1; z.name = L"z.txt"; z.path = L"C:\\fixture\\z.txt"; z.size = 20; z.modified = 200;
            ContentHit a; a.file_id = 2; a.name = L"a.md"; a.path = L"C:\\fixture\\a.md"; a.size = 10; a.modified = 100;
            auto deliver = [&](ContentHit hit) {
                ContentSearchUpdate update; update.progress.generation = request.generation; update.hits.push_back(std::move(hit));
                return session.Accept(std::move(update));
            };
            check(deliver(z).has_value() && deliver(a).has_value(), "cached and later scanned batch accepted");
            const uint64_t first = sort == ContentResultSort::Index ? (descending ? 2 : 1) : (descending ? 1 : 2);
            auto store = session.Results();
            ContentResultStore::Row one, two;
            check(store->Count() == 2 && row(store, 0, one) && row(store, 1, two) &&
                one.file_id == first && two.file_id == 3 - first, "initial pages respect requested sort without UI override");
            auto cancelled = session.Fail(ERROR_CANCELLED);
            check(cancelled && cancelled->progress.error == ERROR_CANCELLED && store->Count() == 2,
                "cancellation retains sorted partial rows");
        }
    }
    for (auto mode : {ContentAgentMode::Observer, ContentAgentMode::LegacyWriter})
        check(!ContentRefreshTestPeer::Queue(saved, mode).task_scan, "non-instant agent retains cached request mode");
    check(!ContentRefreshTestPeer::Queue(saved, ContentAgentMode::Instant, false).task_scan,
        "transient process retains its existing execution mode");
    return failures ? 1 : 0;
}
