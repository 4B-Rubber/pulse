#include "../index/index_engine.h"
#include <filesystem>
#include <cstdio>
#include <algorithm>
namespace pulse::index {
struct EngineTestAccess {
    static bool Fixture(Engine& e, const std::wstring& path) {
        Engine::Store s;
        e.AddNodeLocked(s, -1, L"C:", Engine::kFlagDir);
        e.AddNodeLocked(s, 0, L"A", Engine::kFlagDir);
        e.AddNodeLocked(s, 1, L"leafA.txt", 0);
        e.AddNodeLocked(s, 1, L"folder", Engine::kFlagDir);
        e.AddNodeLocked(s, 3, L"leafChild.txt", 0);
        e.AddNodeLocked(s, 0, L"B", Engine::kFlagDir);
        e.AddNodeLocked(s, 5, L"leafB.txt", 0);
        const uint32_t ends[] = {7,5,3,5,5,7,7};
        for (size_t i = 0; i < s.nodes.size(); ++i) { s.nodes[i].unused = ends[i]; s.attrs[i].size = 5; }
        std::vector<Engine::VolState> volumes(1);
        volumes[0].letter = L'C'; volumes[0].volume_id = L"scope-test";
        volumes[0].root_idx = 0; volumes[0].first_idx = 0; volumes[0].item_count = 7;
        if (!e.WriteIndexFile(path, s, volumes, 123456) || !MoveFileExW((path + L".tmp").c_str(), path.c_str(), 0)) return false;
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!e.MapIndexFile(path, mapped)) return false;
        e.AdoptMappedLocked(std::move(mapped));
        return true;
    }
    static void Move(Engine& e, int32_t id, int32_t parent) {
        const auto node = e.NodeAt(id);
        const std::wstring name(e.NameOf(id));
        e.ChildMapRemove(node.parent, name, id);
        auto& patch = e.patches_[id];
        patch.parent = parent; patch.flags = node.flags; patch.has_meta = true;
        e.ChildMapAdd(parent, name, id);
        e.InvalidateFilterLocked();
    }
    static void Cold(Engine& from, Engine& to) {
        for (int32_t id = 1; id < from.LiveCount(); ++id)
            to.AddForTest(from.BuildPathLocked(id), std::wstring(from.NameOf(id)),
                (from.NodeAt(id).flags & Engine::kFlagDir) != 0, 5);
    }
    static void Clear(Engine& e) { e.InvalidateFilterLocked(); }
};
}
int main() {
    using namespace pulse::index;
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"scope_move_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    if (!fs::create_directory(root)) return 2;
    {
        Engine engine;
        const bool ready = EngineTestAccess::Fixture(engine, (root / L"snapshot.bin").wstring());
        check(ready, "mapped snapshot with DFS spans created");
        if (ready) {
            for (int stage = 0; stage < 5; ++stage) {
                if (stage == 1) EngineTestAccess::Move(engine, 2, 5);
                if (stage == 2) EngineTestAccess::Move(engine, 3, 5);
                if (stage == 3) EngineTestAccess::Move(engine, 2, 1);
                if (stage == 4) EngineTestAccess::Move(engine, 3, 1);
                Engine cold;
                EngineTestAccess::Cold(engine, cold);
                for (const auto* scope : {L"C:\\A", L"C:\\B"}) {
                    for (const auto* term : {L"leaf", L"ext:txt", L"size:>1", L""}) {
                        for (bool page : {false, true}) {
                            Query q; q.path_prefix = scope; q.needle = term; q.rank = false;
                            q.sort = ResultSort::Name; q.offset = page ? 1 : 0; q.limit = page ? 2 : 100;
                            EngineTestAccess::Clear(engine);
                            const auto actual = engine.Search(q), expected = cold.Search(q);
                            const bool same = actual.total == expected.total && std::equal(actual.hits.begin(), actual.hits.end(),
                                expected.hits.begin(), expected.hits.end(), [](const Hit& a, const Hit& b) { return a.path == b.path; });
                            if (!same) printf("[CASE] stage=%d scope=%ls term=%ls page=%d actual=%zu expected=%zu\n", stage, scope, term, page, actual.total, expected.total);
                            check(same, "mapped scope and page equal current-parent-chain cold search");
                        }
                    }
                }
            }
        }
    }
    // Only the newly and exclusively created fixture root is removed.
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "owned mapped snapshot and sidecars cleaned up");
    return failures ? 1 : 0;
}
