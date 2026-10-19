#include "pricing.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nmm {

double fsum(const std::vector<double>& values) {
    std::vector<double> partials;
    for (double x : values) {
        size_t i = 0;
        for (double y : partials) {
            if (std::fabs(x) < std::fabs(y)) std::swap(x, y);
            double hi = x + y;
            double lo = y - (hi - x);
            if (lo != 0.0) partials[i++] = lo;
            x = hi;
        }
        partials.resize(i);
        partials.push_back(x);
    }
    // sum_exact: add the partials from the top, then correct the last rounding (half-way cases)
    size_t n = partials.size();
    double hi = 0.0;
    if (n > 0) {
        hi = partials[--n];
        double lo = 0.0;
        while (n > 0) {
            double x = hi;
            double y = partials[--n];
            hi = x + y;
            double yr = hi - x;
            lo = y - yr;
            if (lo != 0.0) break;
        }
        if (n > 0 && ((lo < 0 && partials[n - 1] < 0) || (lo > 0 && partials[n - 1] > 0))) {
            double y = lo * 2;
            double x = hi + y;
            double yr = x - hi;
            if (y == yr) hi = x;
        }
    }
    return hi;
}

double pstdev(const std::vector<double>& values, double mu) {
    std::vector<double> squares;
    squares.reserve(values.size());
    for (double x : values) {
        double d = x - mu;
        squares.push_back(d * d);
    }
    return std::sqrt(fsum(squares) / static_cast<double>(values.size()));
}

Pricer::~Pricer() = default;

std::optional<double> Pricer::fair(double reference_price) const {
    auto r = ratio();
    if (!r) return std::nullopt;
    return reference_price * *r;
}

// ------------------------------------------------------------------ RatioPricer

RatioPricer::RatioPricer(int window, int min_samples) : Pricer(min_samples), window(window) {
    if (!(1 <= min_samples && min_samples <= window))
        throw std::invalid_argument("RatioPricer needs 1 <= min_samples <= window");
}

void RatioPricer::observe(double venue_price, double reference_price) {
    double r = venue_price / reference_price;
    if (static_cast<int>(ratios_.size()) == window) {
        sum_ -= ratios_.front();  // about to be evicted
        ratios_.pop_front();
    }
    ratios_.push_back(r);
    sum_ += r;
    count += 1;
    if (count % window == 0) sum_ = fsum(std::vector<double>(ratios_.begin(), ratios_.end()));
}

std::optional<double> RatioPricer::ratio() const {
    size_t n = ratios_.size();
    if (static_cast<int>(n) < min_samples) return std::nullopt;
    return sum_ / static_cast<double>(n);
}

std::optional<double> RatioPricer::ratio_std_bps() const {
    auto r = ratio();
    if (!r || ratios_.size() < 2) return std::nullopt;
    return pstdev(std::vector<double>(ratios_.begin(), ratios_.end()), *r) / *r * 1e4;
}

// ------------------------------------------------------------------- EwmaPricer

EwmaPricer::EwmaPricer(double time_constant_s, int min_samples, Clock clock, double min_weight)
    : Pricer(min_samples), time_constant_s(time_constant_s), min_weight(min_weight), clock(std::move(clock)) {
    if (time_constant_s <= 0 || min_samples < 1 || !(0 <= min_weight && min_weight < 1))
        throw std::invalid_argument(
            "EwmaPricer needs time_constant_s > 0, min_samples >= 1 and 0 <= min_weight < 1");
}

void EwmaPricer::observe(double venue_price, double reference_price) {
    double now = clock();
    double r = venue_price / reference_price;
    if (!count) {
        mean_ = r;  // stands for the average until any time has passed
    } else if (paused_) {
        since_ = now;  // the gap counts for nothing
        paused_ = false;
    } else {
        State s = decayed(now);
        mean_ = s.mean;
        var_ = s.var;
        weight_ = s.weight;
    }
    last_ = r;
    since_ = now;
    count += 1;
}

void EwmaPricer::pause() {
    if (paused_ || !count) return;
    State s = decayed(clock());
    mean_ = s.mean;
    var_ = s.var;
    weight_ = s.weight;
    paused_ = true;
}

EwmaPricer::State EwmaPricer::decayed(double now) const {
    if (paused_) return {mean_, var_, weight_};
    double keep = std::exp(-std::max(now - since_, 0.0) / time_constant_s);
    double weight = weight_ * keep + (1.0 - keep);
    if (weight <= 0.0) return {mean_, var_, 0.0};  // no time has passed since the first ratio
    double a = (1.0 - keep) / weight;
    double diff = last_ - mean_;
    return {mean_ + a * diff, (1.0 - a) * (var_ + a * diff * diff), weight};
}

std::optional<double> EwmaPricer::ratio() const {
    if (count < min_samples) return std::nullopt;
    State s = decayed(clock());
    if (!(s.weight >= min_weight)) return std::nullopt;
    return s.mean;
}

std::optional<double> EwmaPricer::ratio_std_bps() const {
    auto r = ratio();
    if (!r) return std::nullopt;
    return std::sqrt(decayed(clock()).var) / *r * 1e4;
}

}  // namespace nmm
