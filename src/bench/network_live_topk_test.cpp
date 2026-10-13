#include <windows.h>
#include <winnetwk.h>
#include "../index/network_index.h"
#include "../index/index_config.h"
#include <iostream>

namespace {
constexpr uint32_t kFixtureCount = 100001;
const std::wstring root_path = L"\\\\pulse-test.invalid\\topk";
uint32_t current = 0;
int handles_open = 0;
bool unexpected_path = false;
DWORD end_error = ERROR_NO_MORE_FILES;
void Fill(WIN32_FIND_DATAW& data) {
    data = {};
    data.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (current + 1 == kFixtureCount) {
        wcscpy_s(data.cFileName, L"needle");
        data.nFileSizeLow = 900001;
        data.ftLastWriteTime.dwLowDateTime = 9000001;
    } else {
        swprintf_s(data.cFileName, L"z-file-%06u-needle.txt", current);
        data.nFileSizeLow = current + 1;
        data.ftLastWriteTime.dwLowDateTime = current + 100;
    }
}
HANDLE WINAPI TopkFindFirst(LPCWSTR path, FINDEX_INFO_LEVELS, LPVOID data,
                            FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    if (std::wstring(path) != L"\\\\?\\UNC\\pulse-test.invalid\\topk\\*") {
        unexpected_path = true;
        SetLastError(ERROR_BAD_NETPATH);
        return INVALID_HANDLE_VALUE;
    }
    current = 0;
    Fill(*static_cast<WIN32_FIND_DATAW*>(data));
    ++handles_open;
    return reinterpret_cast<HANDLE>(new int(1));
}
BOOL WINAPI TopkFindNext(HANDLE, LPWIN32_FIND_DATAW data) {
    if (++current == kFixtureCount) { SetLastError(end_error); return FALSE; }
    Fill(*data);
    return TRUE;
}
BOOL WINAPI TopkFindClose(HANDLE handle) {
    delete reinterpret_cast<int*>(handle);
    --handles_open;
    return TRUE;
}
}
#define FindFirstFileExW TopkFindFirst
#define FindNextFileW TopkFindNext
#define FindClose TopkFindClose
#include "../index/network_index.cpp"
#undef FindClose
#undef FindNextFileW
#undef FindFirstFileExW

int main() {
    using namespace pulse::index;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        failures += !ok;
    };
    const auto walk = [](const Query& query, uint32_t cancel_at = UINT32_MAX) {
        LiveNetworkMatches matches;
        std::mutex mutex;
        current = 0;
        LiveNetworkWalk(query, matches, mutex, [=] { return current >= cancel_at; }, {});
        return matches;
    };
    Query query;
    query.session_id = 7;
    query.path_prefix = root_path;
    query.needle = L"needle";
    query.rank = false;
    query.limit = 1;
    query.sort_desc = true;
    for (auto sort : {ResultSort::Size, ResultSort::Mtime}) {
        query.sort = sort;
        const auto matches = walk(query);
        const auto result = SelectLiveNetworkHits(query, matches);
        check(matches.complete && matches.error == 0 && matches.total == kFixtureCount &&
              matches.hits.size() == kSearchPageCap && result.total == kFixtureCount &&
              result.hits.size() == 1 && result.hits.front().name == L"needle" && handles_open == 0,
              sort == ResultSort::Size ? "size top-1 includes maximum at enumeration position 100001" :
                                        "mtime top-1 includes newest at enumeration position 100001");
        auto page = query; page.offset = 1; page.limit = 3;
        const auto later = SelectLiveNetworkHits(page, matches);
        check(later.total == kFixtureCount && later.hits.size() == 3 &&
              later.hits[0].size == 100000 && later.hits[1].size == 99999 && later.hits[2].size == 99998,
              "same ordering pagination reads globally selected candidates with exact total");
        auto changed = query; changed.sort = ResultSort::Name; changed.sort_desc = false;
        const auto incompatible = SelectLiveNetworkHits(changed, matches);
        check(incompatible.error == ERROR_INVALID_PARAMETER && incompatible.hits.empty(),
              "truncated prefix cannot silently answer a different ordering");
    }
    query.rank = true; query.sort = ResultSort::Index;
    const auto ranked = walk(query);
    const auto best = SelectLiveNetworkHits(query, ranked);
    check(ranked.total == kFixtureCount && ranked.hits.size() == kSearchPageCap &&
          best.hits.size() == 1 && best.hits.front().name == L"needle",
          "rank top-1 includes exact match at enumeration position 100001");
    query.rank = false; query.sort = ResultSort::Name; query.sort_desc = false;
    const auto ascending = walk(query);
    const auto first = SelectLiveNetworkHits(query, ascending);
    check(first.hits.size() == 1 && first.hits.front().name == L"needle",
          "name ascending rescans and retains late smallest name");
    auto descending_query = query; descending_query.sort_desc = true;
    check(!CanReuseLiveNetworkWalk(query, descending_query), "changed sort direction invalidates live cache");
    const auto descending = walk(descending_query);
    const auto last = SelectLiveNetworkHits(descending_query, descending);
    check(last.hits.size() == 1 && last.hits.front().name == L"z-file-099999-needle.txt" &&
          last.total == kFixtureCount, "new ordering produces correct full-scope top-1");
    auto page = query; page.offset = 70; page.limit = 11;
    check(CanReuseLiveNetworkWalk(query, page), "page size and offset reuse the full capped top-k");
    auto session = query; ++session.session_id;
    auto needle = query; needle.needle = L"other";
    auto folder = query; folder.path_prefix += L"\\sub";
    auto folders = query; folders.folders_only = true;
    auto relevance = query; relevance.rank = true;
    auto size = query; size.sort = ResultSort::Size;
    auto subscribe = query; subscribe.subscribe = !subscribe.subscribe;
    check(!CanReuseLiveNetworkWalk(query, session) && !CanReuseLiveNetworkWalk(query, needle) &&
          !CanReuseLiveNetworkWalk(query, folder) && !CanReuseLiveNetworkWalk(query, folders) &&
          !CanReuseLiveNetworkWalk(query, relevance) && !CanReuseLiveNetworkWalk(query, size) &&
          !CanReuseLiveNetworkWalk(query, subscribe),
          "session and every matching/ordering field invalidate live cache");
    const auto cancelled = walk(query, 1000);
    check(!cancelled.complete && cancelled.error == 0 && cancelled.total == 1000 &&
          cancelled.hits.size() == 1000 && handles_open == 0,
          "cancellation inside one large directory keeps bounded partial results and closes handle");
    end_error = ERROR_NETNAME_DELETED;
    const auto partial = walk(query);
    check(partial.complete && partial.error == ERROR_NETNAME_DELETED && partial.total == kFixtureCount &&
          partial.hits.size() == kSearchPageCap && handles_open == 0,
          "enumeration error remains terminal with partial full-scope candidates");
    end_error = ERROR_NO_MORE_FILES;
    query.sort = ResultSort::Index; query.rank = false;
    const auto traversal = walk(query);
    const auto prefix = SelectLiveNetworkHits(query, traversal);
    check(traversal.total == kFixtureCount && traversal.hits.size() == kSearchPageCap &&
          prefix.hits.size() == 1 && prefix.hits.front().name == L"z-file-000000-needle.txt",
          "Index ordering retains traversal prefix while counting every match");
    check(!unexpected_path && handles_open == 0, "synthetic enumeration uses no real files or network shares");
    return failures ? 1 : 0;
}
