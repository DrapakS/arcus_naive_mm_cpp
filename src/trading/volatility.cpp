#include "trading/volatility.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace nmm {

VolScale::VolScale(double short_s, double long_s, double low, double high, double warmup_s, double step_s, Clock clock)
    : short_s(short_s), long_s(long_s), step_s(step_s), low(low), high(high), warmup_s(warmup_s), clock(std::move(clock)) {
    if (!(0 < step_s && step_s <= short_s && short_s < long_s))
        throw std::invalid_argument("volatility needs 0 < step_s <= short_s < long_s");
    if (!(0 < low && low <= 1 && 1 <= high)) throw std::invalid_argument("volatility needs 0 < low <= 1 <= high");
}

void VolScale::observe(double price, std::optional<double> now_) {
    if (!(price > 0)) return;
    double now = now_ ? *now_ : clock();
    if (!last_) {
        last_ = std::pair {now, price};
        since_ = now;
        return;
    }
    double dt = now - last_->first;
    if (dt < step_s) return;
    double steps = dt / step_s;
    double r2 = std::pow(std::log(price / last_->second), 2) / steps;  // per step
    double a_s = 1 - std::exp(-dt / short_s);
    double a_l = 1 - std::exp(-dt / long_s);
    auto& m = sums_;
    m[0] += a_s * (r2 - m[0]);
    m[1] += a_s * (1 - m[1]);
    m[2] += a_l * (r2 - m[2]);
    m[3] += a_l * (1 - m[3]);
    short_var = m[0] / m[1];
    long_var = m[2] / m[3];
    last_ = std::pair {now, price};
    factor_ = work_out(now);
}

Decimal VolScale::work_out(double now) const {
    if (!since_ || now - *since_ < warmup_s || !long_var || !*long_var || !short_var) return Decimal(1);
    double ratio = std::sqrt(*short_var / *long_var);
    // Decimal(str(round(x, 4))): the four places, correctly rounded
    return Decimal::parse(std::format("{:.4f}", std::min(high, std::max(low, ratio))));
}

std::optional<double> VolScale::bp(std::optional<double> var) const {
    if (!var) return std::nullopt;
    return std::sqrt(*var / step_s) * 1e4;
}

std::optional<std::string> VolScale::note() const {
    if (factor_ == 1) return std::nullopt;
    return "volatility x" + factor_.fixed(2);
}

std::string VolScale::status() const {
    auto s = bp(short_var), lg = bp(long_var);
    if (!s || !lg) return "volatility: warming up";
    bool warm = since_ && clock() - *since_ >= warmup_s;
    return std::format("volatility: {:.2f} bp/√s over {:g} s vs {:.2f} over {:g} s: markup x{}", *s, short_s, *lg, long_s,
                       factor_.fixed(2)) +
           (warm ? "" : " (warming up: x1)");
}

}  // namespace nmm
