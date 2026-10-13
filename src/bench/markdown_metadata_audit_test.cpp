#include "../ui/markdown_image_metadata.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

int main() {
    using namespace std::chrono;
    using pulse::ui::MarkdownImageInfo;
    struct Fixture { std::mutex mutex; std::condition_variable cv; bool release = false, entered = false, finished = false; };
    auto fixture = std::make_shared<Fixture>();
    auto& mutex = fixture->mutex;
    auto& cv = fixture->cv;
    auto& release = fixture->release;
    auto& entered = fixture->entered;
    bool ok = true;
    auto resolver = [fixture](const std::wstring& target, const std::wstring&) {
        if (target == L"slow") {
            std::unique_lock lock(fixture->mutex);
            fixture->entered = true; fixture->cv.notify_all();
            fixture->cv.wait(lock, [&] { return fixture->release; });
            fixture->finished = true; fixture->cv.notify_all();
        }
        MarkdownImageInfo result; result.ready = true; result.path = target; return result;
    };
    auto cache = std::make_unique<pulse::ui::MarkdownImageMetadata>(resolver);
    const auto start = steady_clock::now();
    ok &= !cache->Lookup(L"slow", L"base", nullptr).ready;
    ok &= steady_clock::now() - start < milliseconds(100);
    {
        std::unique_lock lock(mutex);
        ok &= cv.wait_for(lock, seconds(2), [&] { return entered; });
    }
    cache->Reset();
    MarkdownImageInfo current;
    const auto deadline = steady_clock::now() + seconds(2);
    do {
        current = cache->Lookup(L"new", L"base", nullptr);
        if (!current.ready) std::this_thread::sleep_for(milliseconds(5));
    } while (!current.ready && steady_clock::now() < deadline);
    ok &= current.ready && current.path == L"new";
    const auto closing = steady_clock::now();
    cache.reset();
    ok &= steady_clock::now() - closing < milliseconds(100);
    {
        std::lock_guard lock(mutex); release = true;
    }
    cv.notify_all();
    {
        std::unique_lock lock(mutex);
        ok &= cv.wait_for(lock, seconds(2), [&] { return fixture->finished; });
    }
    printf("[%s] metadata lookup returns immediately, new generation bypasses stale result, close does not join slow I/O\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
