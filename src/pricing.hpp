// Naive pricing of Arcus off Binance (spot or futures).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "util/clock.hpp"

namespace nmm {

// math.fsum: the correctly rounded sum of the floats (Shewchuk's partials, as CPython does it).
double fsum(const std::vector<double>& values);
// statistics.pstdev(values, mu): the population standard deviation about `mu`.
double pstdev(const std::vector<double>& values, double mu);

class Pricer {
public:
    explicit Pricer(int min_samples) : min_samples(min_samples) {}
    virtual ~Pricer();

    virtual void observe(double venue_price, double reference_price) = 0;
    // The ratio (basis) now; nullopt while warming up.
    virtual std::optional<double> ratio() const = 0;
    std::optional<double> fair(double reference_price) const;
    virtual size_t samples() const = 0;
    // Spread of the ratios in bp of their mean, for reporting.
    virtual std::optional<double> ratio_std_bps() const = 0;
    // No more samples for now (a time-weighted pricer stops its clock).
    virtual void pause() {}
    // A time-weighted pricer (EwmaPricer).
    virtual bool timed() const = 0;

    int min_samples;
};

// Fair venue price = reference price x mean(venue price / reference price).
//
// Assumes the ratio between the venues holds: keeps the last `window` observed
// ratios and prices off their arithmetic mean. No estimate until `min_samples`
// ratios are in.
//
// A running sum makes each step O(1). It is recomputed exactly once per
// `window` samples so float error cannot build up over a long run.
class RatioPricer : public Pricer {
public:
    explicit RatioPricer(int window = 1000, int min_samples = 100);

    void observe(double venue_price, double reference_price) override;
    std::optional<double> ratio() const override;
    size_t samples() const override { return ratios_.size(); }
    // Standard deviation of the ratios in bp of their mean. O(window), for reporting.
    std::optional<double> ratio_std_bps() const override;
    bool timed() const override { return false; }

    int window;
    int64_t count = 0;  // samples seen over the whole run

private:
    std::deque<double> ratios_;
    double sum_ = 0.0;
};

// Fair venue price = reference price x basis, the basis a time-weighted
// exponential moving average of venue price / reference price.
//
// Each ratio counts for as long as it stood, so a burst of updates weighs no
// more than a quiet stretch of the same length: `time_constant_s` is how fast
// the basis follows (about 63% of a step in that time). Feed it on every update
// of either book. No estimate until `min_samples` ratios are in and the ratios
// seen cover `min_weight` of the average (0.1: a tenth of a time constant).
//
// The average is normalised by the weight of what it has seen, so at the start
// it is the plain time-weighted mean of the ratios so far rather than the first
// ratio with almost all the weight. `pause` stops the clock while the venue
// cannot be priced from (its book down or frozen): the last ratio does not go
// on counting for the time nobody saw it.
class EwmaPricer : public Pricer {
public:
    EwmaPricer(double time_constant_s, int min_samples = 100, Clock clock = monotonic, double min_weight = 0.0);

    void observe(double venue_price, double reference_price) override;
    // No more samples for now: fold in the last ratio up to this moment and stop the clock.
    void pause() override;
    std::optional<double> ratio() const override;
    size_t samples() const override { return static_cast<size_t>(count); }
    std::optional<double> ratio_std_bps() const override;
    bool timed() const override { return true; }

    double time_constant_s;
    double min_weight;
    Clock clock;
    int64_t count = 0;

private:
    struct State {
        double mean, var, weight;
    };
    // Mean, variance and weight with the latest ratio held until `now`.
    State decayed(double now) const;

    double mean_ = 0.0;  // the average up to the last observation
    double var_ = 0.0;  // its exponentially weighted variance
    double weight_ = 0.0;  // the weight of the ratios it has seen: 1 - exp(-time seen / time constant)
    double last_ = 0.0;  // the latest ratio, standing since since_
    double since_ = 0.0;
    bool paused_ = false;
};

}  // namespace nmm
