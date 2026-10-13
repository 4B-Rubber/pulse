#include "../index/content_search_session.h"
#include "../../third_party/sqlite/sqlite3.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>

namespace {
std::atomic<uint64_t> row_writes{0};
std::atomic<uint64_t> display_writes{0};

int TrackedOpen(const void* filename, sqlite3** db) {
    const int rc = sqlite3_open16(filename, db);
    if (rc == SQLITE_OK) {
        sqlite3_update_hook(*db, [](void*, int, const char*, const char* table, sqlite3_int64) {
            ++row_writes;
            if (std::strcmp(table, "hits") != 0) ++display_writes;
        }, nullptr);
    }
    return rc;
}
}

// Observe the real store's writes, including any temporary rank table inserts.
// No replacement SQL or production instrumentation is used by this test.
#define sqlite3_open16 TrackedOpen
#include "../index/content_result_store.cpp"
#undef sqlite3_open16

namespace {
using namespace pulse::index;
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!ok) ++failures;
}

ContentHit MakeHit(size_t key, uint64_t id = 0) {
    wchar_t name[40]{};
    swprintf_s(name, L"f%06zu.txt", key);
    ContentHit hit;
    hit.name = name;
    hit.path = L"C:\\synthetic-content\\" + hit.name;
    hit.snippet = L"matching body";
    hit.size = key;
    hit.file_id = id;
    return hit;
}

bool Barrier(const std::shared_ptr<ContentResultStore>& store) {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    store->Resolve({}, false, 0, [done](auto) { done->set_value(); });
    return future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
}

ContentResultStore::Selection Select(const std::shared_ptr<ContentResultStore>& store,
                                     std::vector<int> indices, uint64_t epoch = UINT64_MAX) {
    auto done = std::make_shared<std::promise<ContentResultStore::Selection>>();
    auto future = done->get_future();
    store->Resolve(std::move(indices), false, store->Count(),
                   [done](auto result) { done->set_value(std::move(result)); }, epoch);
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        ContentResultStore::Selection failed;
        failed.error = ERROR_TIMEOUT;
        return failed;
    }
    return future.get();
}

bool RowAt(const std::shared_ptr<ContentResultStore>& store, size_t index, uint64_t size) {
    ContentResultStore::Row row;
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 10000) {
        if (store->Get(index, row)) return row.entry.size == size;
        Sleep(1);
    }
    return false;
}

void PrintPlan(const std::shared_ptr<ContentResultStore>& store) {
    sqlite3* db = nullptr;
    sqlite3_stmt* statement = nullptr;
    bool ok = sqlite3_open16(store->CachePath().c_str(), &db) == SQLITE_OK;
    if (ok) {
        sqlite3_create_collation(db, "PULSE_ORDINAL", SQLITE_UTF16, nullptr, OrdinalCollation);
        sqlite3_create_function_v2(db, "pulse_extension", 1, SQLITE_UTF16 | SQLITE_DETERMINISTIC,
                                   nullptr, SqlExtension, nullptr, nullptr, nullptr);
        ok = sqlite3_prepare_v2(db,
            "EXPLAIN QUERY PLAN SELECT name,path,size FROM hits INDEXED BY hits_display_order "
            "WHERE seq IN (SELECT seq FROM visible_members) "
            "ORDER BY name COLLATE PULSE_ORDINAL ASC,seq ASC LIMIT 256 OFFSET 0", -1,
            &statement, nullptr) == SQLITE_OK;
        if (ok) {
            while (sqlite3_step(statement) == SQLITE_ROW)
                std::printf("[PLAN] %s\n", reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)));
        }
    }
    Check(ok, "inspect streaming page query plan");
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

uint64_t Scale(size_t count) {
    row_writes = 0;
    display_writes = 0;
    ContentSearchRequest request;
    request.generation = count;
    request.paged_results = true;
    request.task_scan = true;
    request.sort = ContentResultSort::Name;
    ContentSearchSession session(request, nullptr, 0);
    auto store = session.Results();
    bool ok = true;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = count; i > 0; --i) {
        ContentSearchUpdate update;
        update.progress.generation = count;
        update.hits.push_back(MakeHit(i, i));
        const auto accepted = session.Accept(std::move(update));
        if (!accepted || accepted->progress.error || store->Count() != count - i + 1) {
            ok = false;
            break;
        }
        // Keep page zero hot, as a visible result pane would during streaming.
        if (i == count) ok = ok && RowAt(store, 0, count);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const uint64_t writes = row_writes.load();
    const uint64_t positions = display_writes.load();
    std::printf("[WORK] hits=%zu row_writes=%llu display_writes=%llu elapsed_ms=%.2f\n", count,
                static_cast<unsigned long long>(writes), static_cast<unsigned long long>(positions), elapsed);
    Check(ok && store->RawCount() == count && store->Error() == 0, "single-hit session publishes every result");
    Check(writes >= count && writes <= count * 8 && positions <= count * 4,
          "single-hit SQLite row writes stay linear");
    const auto selected = Select(store, {0, 255, 256, static_cast<int>(count - 1)});
    Check(selected.error == 0 && selected.rows.size() == 4 &&
          selected.rows[0].second.entry.size == 1 && selected.rows[1].second.entry.size == 256 &&
          selected.rows[2].second.entry.size == 257 && selected.rows[3].second.entry.size == count &&
          RowAt(store, 256, 257), "stream sorting and cross-page positions are exact");
    auto cancelled = session.Fail(ERROR_CANCELLED);
    Check(cancelled && cancelled->progress.done && cancelled->progress.error == ERROR_CANCELLED &&
          cancelled->results == store && store->Count() == count && RowAt(store, 0, 1),
          "cancellation preserves all published partial results");
    Check(store->CachedRows() <= ContentResultStore::kPageSize * ContentResultStore::kCachePages,
          "stream cache remains bounded");
    if (count == 1000) PrintPlan(store);
    return writes;
}

void Mutations() {
    auto store = std::make_shared<ContentResultStore>(nullptr, 0);
    auto a = MakeHit(30), b = MakeHit(10, 10), c = MakeHit(20, 20);
    Check(store->StreamUpsert({a, b, c}, ContentResultSort::Name, false) && RowAt(store, 0, 10),
          "initial stream sorted globally");
    const auto stale = store->OrderRevision();
    a.file_id = 30;
    Check(store->StreamUpsert({a}, ContentResultSort::Name, false) && store->Count() == 3,
          "identity promotion retains single row");
    Check(Select(store, {0}, stale).error == ERROR_CANCELLED, "stale position selection is rejected");
    store->SetSort(ContentResultSort::Index, false);
    Check(Barrier(store) && RowAt(store, 0, 30), "identity promotion retains discovery position");
    a.name = L"renamed.txt";
    a.path = L"C:\\synthetic-content\\renamed.txt";
    a.size = 35;
    Check(store->StreamUpsert({a}, ContentResultSort::Name, false) && store->Count() == 3 && RowAt(store, 0, 35),
          "rename retains identity and requested discovery sort");
    auto identity_done = std::make_shared<std::promise<int>>();
    auto identity_result = identity_done->get_future();
    store->FindIdentity(30, [identity_done](int pos) { identity_done->set_value(pos); });
    Check(identity_result.wait_for(std::chrono::seconds(10)) == std::future_status::ready && identity_result.get() == 0,
          "identity lookup uses current display positions");
    store->SetFilter([](const pulse::fs::DirEntry& entry) { return entry.size >= 20; });
    Check(Barrier(store) && store->Count() == 2, "filter preserves admitted identities");
    b.size = 25;
    c.size = 5;
    Check(store->StreamUpsert({b, c}, ContentResultSort::Name, false) && store->Count() == 2 &&
          RowAt(store, 0, 35) && RowAt(store, 1, 25), "stream updates enter and leave active filter");
    a.removed = true;
    Check(store->StreamUpsert({a}, ContentResultSort::Name, false) && store->RawCount() == 2 &&
          store->Count() == 1 && RowAt(store, 0, 25), "stream removal updates membership and positions");
    store->SetSort(ContentResultSort::Size, true);
    Check(Barrier(store) && RowAt(store, 0, 25), "explicit sort preserves stream filter");
    auto d = MakeHit(40, 40);
    Check(store->StreamUpsert({d}, ContentResultSort::Name, false) && RowAt(store, 0, 40),
          "stream resumes after explicit sort");
    store->SetFilter({});
    Check(Barrier(store) && store->Count() == 3 && RowAt(store, 2, 5), "filter removal restores hidden hits");
    c.size = 50;
    Check(store->ApplyChanges({c}, ContentResultSort::Size, true) && RowAt(store, 0, 50),
          "non-stream changes remain compatible");
    {
        sqlite3* db = nullptr;
        sqlite3_stmt* query = nullptr;
        bool gap = sqlite3_open16(store->CachePath().c_str(), &db) == SQLITE_OK;
        if (gap) sqlite3_create_collation(db, "PULSE_ORDINAL", SQLITE_UTF16, nullptr, OrdinalCollation);
        gap = gap && sqlite3_prepare_v2(db, "SELECT count(*),max(seq),sum(seq=3) FROM hits", -1, &query, nullptr) == SQLITE_OK &&
            sqlite3_step(query) == SQLITE_ROW;
        if (gap) {
            std::printf("[STATE] before Append rows=%lld max_seq=%lld seq3_rows=%lld\n",
                sqlite3_column_int64(query, 0), sqlite3_column_int64(query, 1), sqlite3_column_int64(query, 2));
            gap = sqlite3_column_int64(query, 0) == 3 && sqlite3_column_int64(query, 1) == 3 &&
                  sqlite3_column_int64(query, 2) == 1;
        }
        Check(gap, "append fixture has deleted sequence gap and occupied count-as-sequence");
        sqlite3_finalize(query);
        sqlite3_close(db);
    }
    auto e = MakeHit(60, 60);
    Check(store->Append({e}) && store->Count() == 4 && RowAt(store, 0, 60),
          "legacy append remains compatible after stream");
    store->SetFilter([](const pulse::fs::DirEntry& entry) { return entry.size >= 40; });
    Check(Barrier(store) && store->Count() == 3, "filter hides deletion candidate");
    b.removed = true;
    Check(store->StreamUpsert({b}, ContentResultSort::Name, false) && store->RawCount() == 3 &&
          store->Count() == 3, "deleting hidden hit does not decrement visible count");
    d.file_id = 99;
    Check(store->StreamUpsert({d}, ContentResultSort::Name, false) && store->RawCount() == 3 &&
          store->Count() == 3, "path replacement keeps exact hit and membership counts");
    const auto replaced = Select(store, {2});
    Check(replaced.error == 0 && replaced.rows.size() == 1 && replaced.rows[0].second.file_id == 99,
          "path replacement exposes only replacement identity");
    Check(store->Error() == 0, "mutation paths have no SQLite error");
}

void Rollback() {
    auto store = std::make_shared<ContentResultStore>(nullptr, 0);
    Check(store->StreamUpsert({MakeHit(1, 1)}, ContentResultSort::Name, false), "rollback fixture created");
    sqlite3* db = nullptr;
    bool ready = sqlite3_open16(store->CachePath().c_str(), &db) == SQLITE_OK;
    if (ready) {
        sqlite3_create_collation(db, "PULSE_ORDINAL", SQLITE_UTF16, nullptr, OrdinalCollation);
        ready = sqlite3_exec(db, "CREATE TRIGGER fail_stream BEFORE INSERT ON hits WHEN NEW.file_id=3 "
                                  "BEGIN SELECT RAISE(ABORT,'injected failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    sqlite3_close(db);
    Check(ready, "inject SQLite failure after first hit of a batch");
    if (!ready) return;
    Check(!store->StreamUpsert({MakeHit(2, 2), MakeHit(3, 3)}, ContentResultSort::Name, false) &&
          store->Error() == ERROR_DATABASE_FAILURE && store->RawCount() == 1 && store->Count() == 1 &&
          RowAt(store, 0, 1), "failed transaction rolls back hits, membership and counters");
}
}

int main() {
    const auto one = Scale(1000);
    const auto two = Scale(2000);
    const auto four = Scale(4000);
    Check(two <= one * 3 && four <= two * 3, "doubling hits does not quadruple SQLite writes");
    Mutations();
    Rollback();
    return failures == 0 ? 0 : 1;
}
