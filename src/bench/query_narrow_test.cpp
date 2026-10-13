#include "../index/index_engine.h"
#include "../index/index_query.h"
#include <algorithm>
#include <cstdio>
namespace pulse::index {
struct EngineTestAccess {
    static void Clear(Engine& engine) { engine.InvalidateFilterLocked(); }
};
}
int main() {
    using namespace pulse::index;
    int failed = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failed += !ok; };
    Engine engine;
    for (const auto& name : {L"paper.pdf", L"other.png", L"foo", L"foobar", L"1.txt", L"12.txt", L"中国.txt", L"123.txt"})
        engine.AddForTest(std::wstring(L"C:\\query-fixture\\") + name, name, false, 5);
    const std::vector<std::pair<std::wstring, std::wstring>> transitions = {
        {L"ext:p", L"ext:pdf"}, {L"size:<1", L"size:<10"}, {L"\"foo", L"\"foobar"},
        {L"!1", L"!12"}, {L"ext:pdf;p", L"ext:pdf;png"}, {L"\"foo", L"\"foo\""},
        {L"foo?", L"foo*"}, {L"foo", L"foo | paper"}, {L"size:1", L"size:10"},
        {L"dm:2026-01", L"dm:2026-01-01"}, {L"z", L"zh"}, {L"!zh", L"!zhong"},
        {L"1", L"12"}, {L"中", L"中国"}, {L"1 nopinyin:", L"12 nopinyin:"}
    };
    for (const auto& [before, after] : transitions) {
        Query q; q.limit = 1000; q.rank = false; q.needle = before;
        EngineTestAccess::Clear(engine);
        (void)engine.Search(q);
        q.needle = after;
        const auto incremental = engine.Search(q);
        EngineTestAccess::Clear(engine);
        const auto cold = engine.Search(q);
        auto paths = [](const SearchResult& result) {
            std::vector<std::wstring> out;
            for (const auto& hit : result.hits) out.push_back(hit.path);
            std::sort(out.begin(), out.end()); return out;
        };
        check(incremental.total == cold.total && paths(incremental) == paths(cold), "incremental query equals cold Engine::Search");
    }
    for (const auto& text : {L"ext:pdf", L"size:<10", L"!12", L"\"foobar"}) {
        EngineTestAccess::Clear(engine);
        for (size_t length = 1; length <= std::wstring_view(text).size(); ++length) {
            Query q; q.needle = std::wstring(text, length); q.limit = 1000; q.rank = false;
            const auto incremental = engine.Search(q);
            Engine cold;
            for (const auto& name : {L"paper.pdf", L"other.png", L"foo", L"foobar", L"1.txt", L"12.txt", L"中国.txt", L"123.txt"})
                cold.AddForTest(std::wstring(L"C:\\query-fixture\\") + name, name, false, 5);
            const auto expected = cold.Search(q);
            check(incremental.total == expected.total && std::equal(incremental.hits.begin(), incremental.hits.end(),
                expected.hits.begin(), expected.hits.end(), [](const Hit& a, const Hit& b) { return a.path == b.path; }),
                "every typed prefix equals an independent cold query");
        }
    }
    check(QueryCanNarrow(L"1", L"12"), "positive literal substring extension remains optimized");
    check(QueryCanNarrow(L"中", L"中国"), "literal Unicode extension remains optimized");
    check(!QueryCanNarrow(L"\"foo", L"\"foobar"), "exact name replacement cannot narrow");
    check(!QueryCanNarrow(L"!1", L"!12"), "extending exclusion cannot narrow");
    check(!QueryCanNarrow(L"ext:p", L"ext:pdf"), "extension filter cannot narrow by textual prefix");
    check(!QueryCanNarrow(L"size:<1", L"size:<10"), "expanding numeric bound cannot narrow");
    return failed ? 1 : 0;
}
