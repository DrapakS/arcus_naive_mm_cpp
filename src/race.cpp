#include "race.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "util/pyfmt.hpp"

namespace nmm {

std::pair<bool, double> FirstCopies::arrived(int64_t key, int64_t recv_us) {
    auto it = first_.find(key);
    if (it == first_.end()) {
        first_.emplace(key, recv_us);
        order_.push_back(key);
        if (order_.size() > keep_) {
            first_.erase(order_.front());
            order_.pop_front();
        }
        return {true, 0.0};
    }
    return {false, std::max(static_cast<double>(recv_us - it->second) / 1'000, 0.0)};
}

void FirstCopies::clear() {
    first_.clear();
    order_.clear();
}

LinkLag::LinkLag() : connected_at(monotonic()) {}

void LinkLag::connected() {
    samples.clear();
    sampled_at_ = -1e300;
    connected_at = monotonic();
}

void LinkLag::add(double ms) {
    double now = monotonic();
    if (now - sampled_at_ >= SAMPLE_EVERY_S) {
        sampled_at_ = now;
        samples.emplace_back(now, ms);
        if (samples.size() > 6000) samples.pop_front();
    }
}

std::optional<std::pair<double, size_t>> LinkLag::median(std::optional<double> now_) const {
    double now = now_ ? *now_ : monotonic();
    std::vector<double> recent;
    for (const auto& [at, ms] : samples)
        if (now - at <= SLOW_LINK_WINDOW_S) recent.push_back(ms);
    if (recent.size() < SLOW_LINK_MIN_SAMPLES) return std::nullopt;
    std::sort(recent.begin(), recent.end());  // statistics.median: the mean of the middle two of an even count
    size_t n = recent.size();
    double m = n % 2 ? recent[n / 2] : (recent[n / 2 - 1] + recent[n / 2]) / 2;
    return std::make_pair(m, n);
}

std::optional<std::pair<double, size_t>> LinkLag::judge(double threshold_ms, std::optional<double> now_) const {
    double now = now_ ? *now_ : monotonic();
    if (!threshold_ms || now - connected_at < SLOW_LINK_SETTLE_S || now - slow_at < SLOW_LINK_GAP_S) return std::nullopt;
    auto med = median(now);
    if (!med || med->first <= threshold_ms) return std::nullopt;
    return med;
}

void LinkLag::dropped_for_slowness(std::optional<double> now) {
    slow_at = now ? *now : monotonic();
    ++slow_drops;
}

std::string lag_summary(const std::vector<const LinkLag*>& lags) {
    std::vector<std::string> parts;
    for (const LinkLag* lag : lags) {
        auto med = lag->median();
        parts.push_back(med ? std::format("{:.0f}", med->first) : "-");
    }
    return std::format("behind {}ms", join(parts, "/"));
}

}  // namespace nmm
