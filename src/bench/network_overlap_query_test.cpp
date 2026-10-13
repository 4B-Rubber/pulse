#include <windows.h>
#include <winnetwk.h>
#include "../index/network_index.h"
#include "../index/index_config.h"
#include <filesystem>
#include <iostream>
#include <set>

namespace {
std::wstring fixture_directory;
const std::wstring parent_path = L"\\\\server\\share";
const std::wstring child_path = L"\\\\SERVER\\SHARE\\sub";
struct Item { std::wstring path; uint32_t size; bool directory = false; };
}
namespace pulse::index { std::wstring OverlapTestUserIndexRoot() { return fixture_directory; } }
#define UserIndexRoot OverlapTestUserIndexRoot
#include "../index/network_index.cpp"
#undef UserIndexRoot

namespace pulse::index {
struct NetworkIndexTestAccess {
    static bool Add(NetworkIndex& index, const std::wstring& path, const std::vector<Item>& items) {
        static unsigned next = 0;
        const auto file = fixture_directory + L"\\shard-" + std::to_wstring(++next) + L".bin";
        ShardBuilder builder(file + L".build");
        for (const auto& item : items) {
            WIN32_FIND_DATAW find{};
            const auto name = item.path.substr(item.path.rfind(L'\\') + 1);
            wcscpy_s(find.cFileName, name.c_str());
            find.dwFileAttributes = item.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            find.nFileSizeLow = item.size;
            find.ftLastWriteTime.dwLowDateTime = item.size * 10;
            if (!builder.Add(item.path, find)) return false;
        }
        if (!builder.Save(file)) return false;
        NetworkIndex::RootState root;
        root.info.path = path;
        root.info.online = true;
        root.shard = NetworkIndex::Shard::Open(file);
        if (!root.shard) return false;
        std::lock_guard lock(index.mu_);
        index.roots_.push_back(std::move(root));
        return true;
    }
    static void Overlay(NetworkIndex& index, size_t root_index, const std::wstring& path, uint64_t size, bool removed = false) {
        std::lock_guard lock(index.mu_);
        auto& root = index.roots_.at(root_index);
        if (!root.overlay) root.overlay = std::make_shared<NetworkIndex::Overlay>();
        const auto now = std::chrono::steady_clock::now();
        if (removed) root.overlay->removed.insert_or_assign(path, now);
        else {
            NetworkIndex::Overlay::Entry entry;
            entry.size = size; entry.mtime = size * 10; entry.seen = now;
            root.overlay->entries.insert_or_assign(path, entry);
        }
        root.overlay->view.reset();
    }
    static void Run(NetworkIndex& index) {
        index.running_ = true;
        index.search_thread_ = std::thread([&index] { index.SearchLoop(); });
    }
    static void Online(NetworkIndex& index, size_t root, bool online) {
        std::lock_guard lock(index.mu_);
        index.roots_.at(root).info.online = online;
    }
    static void Remove(NetworkIndex& index, size_t root) {
        std::lock_guard lock(index.mu_);
        index.roots_.erase(index.roots_.begin() + static_cast<std::ptrdiff_t>(root));
    }
};
}
namespace {
using namespace pulse::index;
SearchResult Search(NetworkIndex& index, Query query, bool& completed) {
    static uint32_t next = 0;
    const auto id = ++next;
    index.SearchAsync(query, id);
    const auto deadline = GetTickCount64() + 3000;
    SearchResult result;
    while (GetTickCount64() < deadline) {
        if (index.TakeResult(id, result)) { completed = true; return result; }
        Sleep(1);
    }
    completed = false;
    return result;
}
bool Equal(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool Unique(const std::vector<Hit>& hits) {
    for (size_t i = 0; i < hits.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (Equal(hits[i].path, hits[j].path)) return false;
    return true;
}
}
int main() {
    using namespace pulse::index;
    using Access = NetworkIndexTestAccess;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        failures += !ok;
    };
    const auto temp = std::filesystem::temp_directory_path();
    const auto fixture = temp / (L"pulse-network-overlap-test-" + std::to_wstring(GetCurrentProcessId()) +
                                L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    if (!std::filesystem::create_directory(fixture, ec)) return 1;
    fixture_directory = fixture.wstring();
    {
        NetworkIndex index;
        const bool created = Access::Add(index, parent_path, {
            {parent_path + L"\\Sub", 0, true}, {parent_path + L"\\a.txt", 40},
            {parent_path + L"\\Sub\\x.txt", 1}, {parent_path + L"\\Sub\\y.txt", 2},
            {parent_path + L"\\Submarine\\z.txt", 15}}) &&
            Access::Add(index, child_path, {{child_path + L"\\X.txt", 5}, {child_path + L"\\Y.txt", 6}});
        check(created, "real private parent/child shards created and mapped");
        if (created) {
            Access::Overlay(index, 0, parent_path + L"\\Sub\\x.txt", 70);
            Access::Overlay(index, 0, parent_path + L"\\Sub\\new.txt", 7);
            Access::Overlay(index, 1, child_path + L"\\x.txt", 101);
            Access::Overlay(index, 1, child_path + L"\\y.txt", 0, true);
            Access::Overlay(index, 1, child_path + L"\\new.txt", 25);
            Access::Run(index);
            for (const auto sort : {ResultSort::Index, ResultSort::Name, ResultSort::Size, ResultSort::Mtime}) {
                for (bool descending : {false, true}) {
                    Query query; query.rank = false; query.sort = sort; query.sort_desc = descending; query.limit = 2;
                    std::vector<Hit> combined;
                    bool valid = true;
                    for (size_t offset = 0; offset <= 6; offset += 2) {
                        query.offset = offset;
                        bool completed = false;
                        auto page = Search(index, query, completed);
                        valid = valid && completed && page.total == 5 && page.hits.size() == (offset < 4 ? 2 : offset == 4 ? 1 : 0);
                        for (auto& hit : page.hits) combined.push_back(std::move(hit));
                    }
                    const auto compiled = ParseQuery(query.needle);
                    valid = valid && Unique(combined) && (sort == ResultSort::Index ||
                        std::is_sorted(combined.begin(), combined.end(), [&](const Hit& a, const Hit& b) {
                            return BetterHit(a, b, query, compiled);
                        }));
                    valid = valid && std::any_of(combined.begin(), combined.end(), [&](const Hit& hit) {
                        return Equal(hit.path, child_path + L"\\x.txt") && hit.size == 101;
                    }) && std::none_of(combined.begin(), combined.end(), [&](const Hit& hit) {
                        return Equal(hit.path, child_path + L"\\y.txt");
                    });
                    check(valid, "all sorted pages have unique total, fresh child overlay and no deleted parent copy");
                }
            }
            Query query; query.rank = true; query.needle = L"txt"; query.limit = 2; query.offset = 2;
            bool completed = false;
            auto ranked = Search(index, query, completed);
            check(completed && ranked.total == 4 && ranked.hits.size() == 2 && Unique(ranked.hits),
                  "ranked pagination counts each overlapping file once before top-k");
            query = {}; query.rank = false; query.limit = 20; query.path_prefix = child_path;
            auto scoped = Search(index, query, completed);
            check(completed && scoped.total == 3 && Unique(scoped.hits) &&
                  std::count_if(scoped.hits.begin(), scoped.hits.end(), [](const Hit& hit) { return hit.is_dir; }) == 1,
                  "child scope includes its root directory once and excludes prefix lookalike");
            Access::Online(index, 0, false);
            query.path_prefix.clear();
            auto child_only = Search(index, query, completed);
            check(completed && child_only.total == 2 && Unique(child_only.hits),
                  "available child serves independently when parent is offline");
            Access::Online(index, 0, true); Access::Online(index, 1, false);
            auto parent_only = Search(index, query, completed);
            check(completed && parent_only.total == 6 && Unique(parent_only.hits),
                  "available parent fully covers offline child region");
            Access::Online(index, 1, true);
            check(Access::Add(index, L"\\\\server\\share\\SUB", {{child_path + L"\\x.txt", 999}}),
                  "case-equivalent root fixture created");
            auto equivalent = Search(index, query, completed);
            check(completed && equivalent.total == 5 && Unique(equivalent.hits) &&
                  std::none_of(equivalent.hits.begin(), equivalent.hits.end(), [](const Hit& hit) { return hit.size == 999; }),
                  "case-equivalent active roots choose stable first owner");
            Access::Remove(index, 2); Access::Remove(index, 1);
            auto removed = Search(index, query, completed);
            check(completed && removed.total == 6 && Unique(removed.hits),
                  "removing child restores complete parent coverage");
            check(Access::Add(index, child_path, {{child_path + L"\\x.txt", 5}, {child_path + L"\\y.txt", 6}}),
                  "independent child shard restored");
            Access::Remove(index, 0);
            auto parent_removed = Search(index, query, completed);
            check(completed && parent_removed.total == 2 && Unique(parent_removed.hits),
                  "removing parent preserves complete child coverage");
        }
    }
    std::error_code guard_error;
    const bool same_parent = std::filesystem::equivalent(fixture.parent_path(), temp, guard_error);
    const auto status = std::filesystem::symlink_status(fixture, ec);
    const bool owned = same_parent && !guard_error && !ec &&
        status.type() == std::filesystem::file_type::directory &&
        fixture.filename().wstring().starts_with(L"pulse-network-overlap-test-");
    if (owned) std::filesystem::remove_all(fixture, ec);
    check(owned && !ec && !std::filesystem::exists(fixture), "private mapped shard fixture cleaned up");
    return failures ? 1 : 0;
}
