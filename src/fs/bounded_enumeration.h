#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace pulse::fs {
class EnumerationCancelled : public std::runtime_error {
public:
    EnumerationCancelled() : std::runtime_error("Directory enumeration cancelled") {}
};

// Separate quotas keep stalled network providers from consuming local capacity.
// State belongs to the provider thread until its I/O has actually completed.
inline std::atomic<unsigned> active_local_enumerations{0};
inline std::atomic<unsigned> active_network_enumerations{0};

template<class Result, class Provider, class Cancelled>
Result RunBoundedEnumeration(bool network, Provider provider, Cancelled cancelled,
    std::chrono::milliseconds budget = std::chrono::seconds(15)) {
    if (cancelled()) throw EnumerationCancelled();
    auto* active = network ? &active_network_enumerations : &active_local_enumerations;
    if (active->fetch_add(1) >= 4) {
        --*active;
        throw std::runtime_error("Directory provider capacity exhausted");
    }
    struct State {
        std::atomic_bool cancel{false};
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        std::optional<Result> result;
        std::exception_ptr error;
    };
    std::shared_ptr<State> state;
    std::thread thread;
    try {
        state = std::make_shared<State>();
        thread = std::thread([state, active, provider = std::move(provider)]() mutable {
            try { state->result.emplace(provider(state->cancel)); }
            catch (...) { state->error = std::current_exception(); }
            {
                std::lock_guard lock(state->mutex);
                state->done = true;
            }
            --*active;
            state->ready.notify_one();
        });
    } catch (...) { --*active; throw; }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    try {
        std::unique_lock lock(state->mutex);
        for (;;) {
            // Check cancellation before accepting even an already completed result.
            if (cancelled()) throw EnumerationCancelled();
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Directory enumeration timed out");
            if (state->done) break;
            state->ready.wait_for(lock, std::chrono::milliseconds(20));
        }
        lock.unlock();
        thread.join();
    } catch (...) {
        state->cancel = true;
        CancelSynchronousIo(thread.native_handle());
        thread.detach();
        throw;
    }
    if (state->error) std::rethrow_exception(state->error);
    return std::move(*state->result);
}
} // namespace pulse::fs
