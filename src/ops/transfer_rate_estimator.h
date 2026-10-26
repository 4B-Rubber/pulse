#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace pulse::ops {

// Speed and ETA for a running operation, both fed by one sample stream. They
// answer different questions: the readout has to follow what is happening now,
// while the countdown must not jump on every short stall. So the readout is the
// average over a short window (and reads zero once progress stops), and the ETA
// keeps a long window plus smoothing.
class TransferRateEstimator {
public:
    // Byte transfers only trust rates above 1 B/s (below that a countdown is noise);
    // item counts run far slower, so they drop the floor to keep their ETA alive.
    void SetEtaFloor(double floor) { eta_floor_ = floor; }

    // Readout window. Item counts arrive far less often than byte progress and a
    // handful of them is not a rate, so the readout stays empty until `minimum`
    // units have moved.
    void SetDisplayWindow(ULONGLONG window_ms, uint64_t minimum) {
        display_window_ms_ = window_ms != 0 ? window_ms : kDefaultDisplayWindowMs;
        display_minimum_ = minimum;
    }

    void Reset(ULONGLONG tick, uint64_t amount) {
        samples_.clear();
        samples_.push_back({ tick, amount });
        eta_speed_ = 0.0;
        eta_seconds_ = 0;
        last_rate_tick_ = tick;
        last_eta_tick_ = tick;
    }

    void Observe(ULONGLONG tick, uint64_t amount, uint64_t total_amount) {
        if (samples_.empty() || amount < samples_.back().amount) {
            Reset(tick, amount);
            return;
        }

        if (tick - samples_.back().tick < kMinimumSampleMs && amount < total_amount)
            return;

        if (tick == samples_.back().tick) {
            samples_.back().amount = amount;
        } else {
            samples_.push_back({ tick, amount });
        }
        const ULONGLONG retain = (std::max)(kEtaWindowMs, display_window_ms_);
        while (samples_.size() > 2 && samples_[1].tick + retain <= tick)
            samples_.pop_front();

        const ULONGLONG span = tick - samples_.front().tick;
        const uint64_t amount_delta = amount - samples_.front().amount;
        if (span < kWarmupMs) return;
        if (amount_delta == 0) {
            // Nothing moved inside the window. Keep no old rate around: it would
            // freeze the readout and the countdown at the speed of a transfer
            // that already stopped.
            eta_speed_ = 0.0;
            eta_seconds_ = 0;
            return;
        }

        const double window_speed = static_cast<double>(amount_delta) * 1000.0
            / static_cast<double>(span);
        if (eta_speed_ <= 0.0) {
            eta_speed_ = window_speed;
        } else {
            const double seconds = static_cast<double>(tick - last_rate_tick_) / 1000.0;
            const double alpha = 1.0 - std::exp(-seconds);
            eta_speed_ += alpha * (window_speed - eta_speed_);
        }
        last_rate_tick_ = tick;

        if (amount >= total_amount || eta_speed_ <= eta_floor_) {
            eta_seconds_ = 0;
            return;
        }
        if (eta_seconds_ != 0 && tick - last_eta_tick_ < 1000) return;

        const double raw_eta = static_cast<double>(total_amount - amount) / eta_speed_;
        if (eta_seconds_ == 0) {
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::ceil(raw_eta)));
        } else {
            // React faster to a slowdown than to a transient speed-up.
            const double alpha = raw_eta > static_cast<double>(eta_seconds_) ? 0.45 : 0.20;
            const double blended = static_cast<double>(eta_seconds_)
                + alpha * (raw_eta - static_cast<double>(eta_seconds_));
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::llround(blended)));
        }
        last_eta_tick_ = tick;
    }

    // Live readout at `now`: the average over the display window, and zero once
    // progress has stopped for a second.
    double speed(ULONGLONG now) const {
        if (samples_.empty()) return 0.0;
        const Sample& last = samples_.back();
        if (now < last.tick) now = last.tick;
        if (now - last.tick > kStallMs) return 0.0;
        const ULONGLONG cutoff = now > display_window_ms_ ? now - display_window_ms_ : 0;
        const Sample* from = &samples_.front();
        for (const Sample& sample : samples_) {
            if (sample.tick > cutoff) break;
            from = &sample;
        }
        const ULONGLONG span = last.tick - from->tick;
        if (span == 0) return 0.0;
        const uint64_t amount_delta = last.amount - from->amount;
        if (amount_delta == 0 || amount_delta < display_minimum_) return 0.0;
        return static_cast<double>(amount_delta) * 1000.0
            / static_cast<double>(span);
    }

    // Countdown at `now`. Without a live readout there is no estimate: a stalled
    // transfer would otherwise keep counting down from a rate that stopped.
    uint64_t eta_seconds(ULONGLONG now) const {
        return speed(now) > 0.0 ? eta_seconds_ : 0;
    }

private:
    static constexpr ULONGLONG kMinimumSampleMs = 100;
    static constexpr ULONGLONG kEtaWindowMs = 4000;
    static constexpr ULONGLONG kWarmupMs = 750;
    static constexpr ULONGLONG kStallMs = 1000;
    static constexpr ULONGLONG kDefaultDisplayWindowMs = 1500;

    struct Sample {
        ULONGLONG tick = 0;
        uint64_t amount = 0;
    };

    std::deque<Sample> samples_;
    double eta_floor_ = 1.0;
    double eta_speed_ = 0.0;  // long window + smoothing, ETA only
    uint64_t eta_seconds_ = 0;
    ULONGLONG last_rate_tick_ = 0;
    ULONGLONG last_eta_tick_ = 0;
    ULONGLONG display_window_ms_ = kDefaultDisplayWindowMs;
    uint64_t display_minimum_ = 0;
};

} // namespace pulse::ops
