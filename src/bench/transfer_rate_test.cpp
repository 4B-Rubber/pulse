// Rate readout: follows a speed change quickly, reads zero once progress stops,
// and keeps item counts from producing a rate out of a handful of entries.
#include "../ops/transfer_rate_estimator.h"
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += ok ? 0 : 1;
}

// Feeds the estimator the way a transfer does: one sample every 50 ms.
void Feed(pulse::ops::TransferRateEstimator& rate, ULONGLONG start, ULONGLONG end,
          double units_per_s, uint64_t& amount, uint64_t total) {
    for (ULONGLONG t = start + 50; t <= end; t += 50) {
        amount += static_cast<uint64_t>(units_per_s * 0.05);
        rate.Observe(t, amount, total);
    }
}
} // namespace

int main() {
    using pulse::ops::TransferRateEstimator;
    const uint64_t total = 1000ull * 1000 * 1000;

    {
        TransferRateEstimator rate;
        rate.SetDisplayWindow(1500, 0);
        rate.Reset(0, 0);
        uint64_t amount = 0;
        Feed(rate, 0, 3000, 100e6, amount, total);
        const double fast = rate.speed(3000);
        Feed(rate, 3000, 5000, 20e6, amount, total);
        const double slower = rate.speed(5000);
        Check(fast > 80e6 && fast < 110e6, "a steady transfer reads close to its real rate");
        Check(slower < 40e6, "the readout follows a slowdown within the short window");
    }

    {
        // The worker keeps publishing the same amount while a copy is stuck.
        TransferRateEstimator rate;
        rate.SetDisplayWindow(1500, 0);
        rate.Reset(0, 0);
        uint64_t amount = 0;
        Feed(rate, 0, 2000, 100e6, amount, total);
        Check(rate.speed(2000) > 80e6, "the transfer reads its rate before the stall");
        for (ULONGLONG t = 2050; t <= 4000; t += 50) rate.Observe(t, amount, total);
        Check(rate.speed(4000) == 0.0, "a stalled transfer reads zero instead of its last rate");
        Check(rate.eta_seconds(4000) == 0, "a stalled transfer stops the countdown");
    }

    {
        // Nothing arrives at all (a hosed provider): only the clock moves.
        TransferRateEstimator rate;
        rate.SetDisplayWindow(1500, 0);
        rate.Reset(0, 0);
        uint64_t amount = 0;
        Feed(rate, 0, 2000, 100e6, amount, total);
        Check(rate.speed(4000) == 0.0, "no update for a second also reads zero");
    }

    {
        TransferRateEstimator items;
        items.SetEtaFloor(0.0);
        items.SetDisplayWindow(2000, 5);
        items.Reset(0, 0);
        for (uint64_t done = 1; done <= 3; ++done) items.Observe(done * 300, done, 100);
        Check(items.speed(900) == 0.0, "a handful of deleted items is not a rate");
        uint64_t done = 3;
        for (ULONGLONG t = 1200; t <= 3000; t += 200) items.Observe(t, ++done, 200);
        Check(items.speed(3000) > 0.0, "an item rate appears once enough items moved");
        Check(items.eta_seconds(3000) > 0, "the item basis keeps a countdown");
    }

    {
        TransferRateEstimator eta;
        eta.SetDisplayWindow(1500, 0);
        eta.Reset(0, 0);
        uint64_t amount = 0;
        Feed(eta, 0, 4000, 50e6, amount, total);
        Check(eta.eta_seconds(4000) > 0, "a steady byte transfer keeps a countdown");
        Feed(eta, 4000, 6000, 50e6, amount, total);
        Check(eta.eta_seconds(6000) > 0 && eta.speed(6000) > 40e6,
            "the countdown survives a steady run");
    }

    std::printf("\n%s\n", failures ? "transfer rate tests: FAIL" : "transfer rate tests: PASS");
    return failures ? 1 : 0;
}
