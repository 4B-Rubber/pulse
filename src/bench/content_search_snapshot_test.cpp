#include "../app/content_search_snapshot.h"
#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&](bool pass, const char* label) {
        printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
        failures += !pass;
    };
    auto rows = std::make_shared<std::vector<pulse::fs::DirEntry>>();
    for (int i = 0; i < 5; ++i) {
        pulse::fs::DirEntry entry;
        entry.name = L"directory-row-" + std::to_wstring(i);
        entry.full_path = L"C:\\private-memory-fixture\\" + entry.name;
        rows->push_back(std::move(entry));
    }
    const auto empty = std::make_shared<const std::vector<pulse::fs::DirEntry>>();
    constexpr auto query = L"pulse:search:content:X";
    auto first = pulse::app::PrepareContentSearchSnapshot(rows, L"C:\\private-memory-fixture", query, empty);
    check(!first.retained && first.snapshot == empty && first.snapshot->empty(),
          "directory to first content query clears unrelated directory rows before first batch");
    // CancelActiveContentSearch leaves the displayed snapshot unchanged if no
    // result store has arrived. The prepared snapshot must already be safe.
    const auto cancelled = first.snapshot;
    check(cancelled->empty(), "cancel before first batch cannot expose unrelated directory rows");
    check(rows->size() == 5, "clearing the display does not mutate the original directory snapshot");

    auto same = pulse::app::PrepareContentSearchSnapshot(rows, query, query, empty);
    check(same.retained && same.snapshot == rows, "same-query refresh preserves published snapshot identity");
    check(same.snapshot->size() == 5, "cancelling same-query refresh retains actual previous result rows");
    auto changed = pulse::app::PrepareContentSearchSnapshot(rows, query, L"pulse:search:content:Y", empty);
    check(!changed.retained && changed.snapshot == empty,
          "different content query cannot retain the previous query's rows");
    auto no_origin = pulse::app::PrepareContentSearchSnapshot(rows, L"", query, empty);
    check(!no_origin.retained && no_origin.snapshot == empty, "unknown snapshot origin is not a matching search");
    auto no_rows = pulse::app::PrepareContentSearchSnapshot(nullptr, query, query, empty);
    check(!no_rows.retained && no_rows.snapshot == empty, "missing snapshot starts with empty result rows");
    auto empty_rows = pulse::app::PrepareContentSearchSnapshot(empty, query, query, empty);
    check(!empty_rows.retained && empty_rows.snapshot->empty(), "empty same-query result remains empty and not retained");
    auto partial = pulse::app::PrepareContentSearchSnapshot(rows, query, query, empty);
    check(partial.retained && partial.snapshot->front().full_path == rows->front().full_path,
          "same-query partial published results remain usable on cancellation");
    printf("[INFO] Shared production snapshot transition, memory-only; no async message-loop simulation\n");
    return failures ? 1 : 0;
}
