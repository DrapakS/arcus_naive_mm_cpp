// The markup scaled by volatility: wider while the market moves more than it usually does.
//
// The reference mid (Binance futures; the spot book without it) is sampled once a `step_s`; each
// sample's squared log return goes into two exponential means, a short one (`short_s`, now) and a
// long one (`long_s`, the market's usual). The markup factor is sqrt(short / long), within `low` ..
// `high`: x1 when the market moves as usual, more in a burst, less while it is quiet. A gap of
// several steps counts as one return over its length (per step: its square over the steps). Both
// means start from nothing and are divided by the weight they have gathered (bias-corrected), so a
// few minutes after start the long one is the mean of those minutes, not a fraction of it. Until
// `warmup_s` the factor is 1.
//
// 19 Oct, 6 h of Arcus prices at 60 s / 30 min: the factor's mean x1.00 (BTC) and x1.04 (ETH),
// median x0.87 / x0.94, x1.43-1.46 at p90; at x0.8 31-41% of the time, at x2 1-2%: about the markup
// as it was on average, less in quiet spells, more when a move is on. No taker, no stop-loss: only
// where the quotes stand.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>

#include "util/clock.hpp"
#include "util/decimal.hpp"

namespace nmm {

class VolScale {
public:
    // Throws std::invalid_argument unless 0 < step_s <= short_s < long_s and 0 < low <= 1 <= high.
    VolScale(double short_s = 60.0, double long_s = 1800.0, double low = 0.8, double high = 2.0, double warmup_s = 300.0,
             double step_s = 1.0, Clock clock = monotonic);

    // A reference mid: a sample once a step_s has gone by since the last one.
    void observe(double price, std::optional<double> now = {});
    // The markup factor now, low..high (1 until warmed up).
    Decimal factor() const { return factor_; }
    // A variance per step as bp per sqrt(second).
    std::optional<double> bp(std::optional<double> var) const;
    // For the placement line, when it changes the markup.
    std::optional<std::string> note() const;
    std::string status() const;

    double short_s, long_s, step_s;
    double low, high, warmup_s;
    double power = 0.5;  // the factor is (short / long) ^ power: 0.5 the ratio of the volatilities, more reaches low and high sooner
    Clock clock;
    std::optional<double> short_var;  // bias-corrected means of the squared returns, per step
    std::optional<double> long_var;

private:
    Decimal work_out(double now) const;

    std::array<double, 4> sums_ {};  // short mean, its weight, long mean, its weight (from 0)
    std::optional<std::pair<double, double>> last_;  // (when, price) of the last sample
    std::optional<double> since_;  // when the first sample was taken
    Decimal factor_ {1};
};

}  // namespace nmm
