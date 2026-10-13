#include "../index/index_engine.h"
#include <filesystem>
#include <cstdio>
namespace pulse::index {
struct EngineTestAccess {
    static bool Fixture(Engine& e, const std::wstring& path, int count) {
        Engine::Store s;
        e.AddNodeLocked(s, -1, L"C:", Engine::kFlagDir);
        e.AddNodeLocked(s, 0, L"paper.pdf", 0);
        for (int i = 2; i < count; ++i) e.AddNodeLocked(s, 0, L"zz" + std::to_wstring(i), 0);
        std::vector<Engine::VolState> volumes;
        if (!e.WriteIndexFile(path, s, volumes, 123456) ||
            !MoveFileExW((path + L".tmp").c_str(), path.c_str(), 0)) return false;
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!e.MapIndexFile(path, mapped)) return false;
        e.AdoptMappedLocked(std::move(mapped));
        return true;
    }
    static void Patch(Engine& e, int stage) {
        if (stage == 1) {
            auto& patch = e.patches_[1];
            patch.has_attr = true;
            patch.attr = e.AttrAt(1);
            patch.attr.size = 100;
        }
        if (stage == 2) {
            const std::wstring name = L"PAPER.PDF";
            auto& patch = e.patches_[1];
            patch.off = static_cast<uint32_t>(e.live_.pool.size());
            patch.len = static_cast<uint16_t>(name.size());
            e.live_.pool.insert(e.live_.pool.end(), name.begin(), name.end());
            patch.has_name = true;
        }
        e.InvalidateFilterLocked();
    }
};
}
int main() {
    using namespace pulse::index;
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"patch_unique_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    if (!fs::create_directory(root)) return 2;
    for (int count : {3, 65536}) {
        Engine engine;
        const bool ready = EngineTestAccess::Fixture(engine, (root / (std::to_wstring(count) + L".bin")).wstring(), count);
        check(ready, "mapped sparse/dense fixture created");
        if (!ready) continue;
        for (int stage = 0; stage < 3; ++stage) {
            EngineTestAccess::Patch(engine, stage);
            for (const auto* term : {L"a", L"\"paper.pdf\"", L"paper nopinyin:"}) {
                for (bool rank : {false, true}) {
                    for (size_t offset : {0u, 1u}) {
                        EngineTestAccess::Patch(engine, 0);
                        Query q; q.needle = term; q.rank = rank; q.offset = offset; q.limit = 2;
                        const auto result = engine.Search(q);
                        const bool correct = result.total == 1 && result.hits.size() == (offset ? 0u : 1u);
                        if (!correct) printf("[CASE] n=%d stage=%d term=%ls rank=%d offset=%zu total=%zu hits=%zu\n",
                            count, stage, term, rank, offset, result.total, result.hits.size());
                        check(correct, "patched result total and pagination remain unique");
                    }
                }
            }
        }
    }
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "owned fixtures cleaned up");
    return failures ? 1 : 0;
}
