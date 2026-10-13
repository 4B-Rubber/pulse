#include "../index/index_engine.h"
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <future>

namespace pulse::index {
struct EngineTestAccess {
    static void Seed(Engine& engine) {
        std::unique_lock lock(engine.mutex_);
        const auto root = engine.AddNodeLocked(engine.live_, -1, L"C:", Engine::kFlagDir);
        for (const auto* name : {L"A", L"AB", L"B"}) {
            const auto dir = engine.AddNodeLocked(engine.live_, root, name, Engine::kFlagDir);
            for (int i = 0; i < 12; ++i)
                engine.AddNodeLocked(engine.live_, dir, L"leaf" + std::to_wstring(i), 0);
        }
        engine.ready_ = true;
        engine.building_ = false;
    }
    static void Publish(Engine& engine, const IndexConfig& config,
                        const std::function<void()>& action = {}) {
        engine.PublishExcludedPaths(config, action);
    }
    static void ReadLocked(Engine& engine, const std::function<void()>& action) {
        std::shared_lock lock(engine.mutex_);
        action();
    }
    // Called only inside ReadLocked: the same matcher and lock as production readers.
    static bool Excluded(Engine& engine, std::wstring_view path) {
        return engine.IsExcludedPath(path);
    }
};
}

using namespace pulse::index;
using namespace std::chrono_literals;

static std::vector<std::wstring> Paths(const FileFeedPage& page) {
    std::vector<std::wstring> paths;
    for (const auto& record : page.records) paths.push_back(record.path);
    return paths;
}

int main() {
    const auto started = std::chrono::steady_clock::now();
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    std::puts("[INFO] Memory-only Engine; no Start, Worker, FullRebuild, config IO, volumes or snapshots.");
    Engine engine;
    EngineTestAccess::Seed(engine);
    std::array<IndexConfig, 3> configs;
    configs[0].excluded_paths = {L"c:\\a"};
    // Force vector/string replacement between materially different configurations.
    for (int i = 0; i < 192; ++i)
        configs[1].excluded_paths.push_back(L"Z:\\not-in-fixture\\" + std::wstring(240, L'x') + std::to_wstring(i));
    configs[1].excluded_paths.push_back(L"C:\\B");
    std::array<std::vector<std::wstring>, 3> expected;
    for (size_t i = 0; i < configs.size(); ++i) {
        for (const auto* name : {L"A", L"AB", L"B"}) {
            if ((i == 0 && std::wstring_view(name) == L"A") ||
                (i == 1 && std::wstring_view(name) == L"B")) continue;
            for (int leaf = 0; leaf < 12; ++leaf)
                expected[i].push_back(L"C:\\" + std::wstring(name) + L"\\leaf" + std::to_wstring(leaf));
        }
        EngineTestAccess::Publish(engine, configs[i]);
        const auto page = engine.ReadFeed(false, L"", 0, 0);
        check(page.ready && page.done && !page.gap && Paths(page) == expected[i],
              "real ReadFeed observes full expected configuration, including empty reset");
    }
    check(expected[0] != expected[1] && expected[1] != expected[2], "oracle configurations have distinct visible identities");
    EngineTestAccess::Publish(engine, configs[0]);

    std::promise<void> reader_held, inspect, inspected, release_reader, writer_entered;
    auto inspect_signal = inspect.get_future();
    auto release_signal = release_reader.get_future();
    std::atomic<bool> old_consistent{false};
    auto held_reader = std::async(std::launch::async, [&] {
        EngineTestAccess::ReadLocked(engine, [&] {
            reader_held.set_value();
            inspect_signal.wait();
            old_consistent = EngineTestAccess::Excluded(engine, L"C:\\A\\leaf0") &&
                !EngineTestAccess::Excluded(engine, L"C:\\AB\\leaf0") &&
                !EngineTestAccess::Excluded(engine, L"C:\\B\\leaf0");
            inspected.set_value();
            release_signal.wait();
        });
    });
    reader_held.get_future().wait();
    auto writer = std::async(std::launch::async, [&] {
        writer_entered.set_value();
        EngineTestAccess::Publish(engine, configs[1]);
    });
    writer_entered.get_future().wait();
    const bool waiting = writer.wait_for(100ms) == std::future_status::timeout;
    inspect.set_value();
    inspected.get_future().wait();
    check(waiting && old_consistent, "held production matcher retains old batch while publication waits for shared reader");
    release_reader.set_value();
    held_reader.get();
    writer.get();
    check(Paths(engine.ReadFeed(false, L"", 0, 0)) == expected[1], "reader release admits publication and next real page sees complete new batch");

    std::promise<void> publication_held, release_publication, page_entered;
    auto publication_release = release_publication.get_future();
    auto rebuilding = std::async(std::launch::async, [&] {
        EngineTestAccess::Publish(engine, configs[0], [&] {
            publication_held.set_value();
            publication_release.wait();
        });
    });
    publication_held.get_future().wait();
    auto reading = std::async(std::launch::async, [&] {
        page_entered.set_value();
        return engine.ReadFeed(false, L"", 0, 0);
    });
    page_entered.get_future().wait();
    check(reading.wait_for(100ms) == std::future_status::timeout,
          "real ReadFeed waits until configuration and rebuild memory action both leave exclusive section");
    release_publication.set_value();
    rebuilding.get();
    check(Paths(reading.get()) == expected[0], "blocked real reader resumes with complete newly published identities");

    constexpr int rounds = 400;
    constexpr int reader_count = 4;
    std::barrier phase(reader_count + 1);
    std::atomic<unsigned> bad_pages{0}, pages{0};
    std::array<std::thread, reader_count> readers;
    for (auto& reader : readers) reader = std::thread([&] {
        for (int round = 0; round < rounds; ++round) {
            phase.arrive_and_wait();
            for (int sample = 0; sample < 4; ++sample) {
                const auto page = engine.ReadFeed(false, L"", 0, 0);
                const auto paths = Paths(page);
                if (!page.ready || !page.done || page.gap ||
                    std::find(expected.begin(), expected.end(), paths) == expected.end()) ++bad_pages;
                ++pages;
                std::this_thread::yield();
            }
            phase.arrive_and_wait();
        }
    });
    for (int round = 0; round < rounds; ++round) {
        phase.arrive_and_wait();
        EngineTestAccess::Publish(engine, configs[static_cast<size_t>(round) % configs.size()]);
        phase.arrive_and_wait();
    }
    for (auto& reader : readers) reader.join();
    check(pages == static_cast<unsigned>(rounds * reader_count * 4) && bad_pages == 0,
          "bounded interleaving: every real feed page equals one whole exclusion configuration");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("[INFO] rounds=%d readers=%d pages=%u invalid=%u elapsed_ms=%lld; lock barriers plus stress, not a race detector\n",
                rounds, reader_count, pages.load(), bad_pages.load(), static_cast<long long>(elapsed));
    return failures ? 1 : 0;
}
