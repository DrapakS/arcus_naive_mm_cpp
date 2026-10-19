#include "health.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <random>

#include "util/clock.hpp"

namespace nmm {

namespace {

double pct(const std::vector<double>& values, double q) {
    size_t i = std::min(values.size() - 1, static_cast<size_t>(q * static_cast<double>(values.size())));
    return values[i];
}

std::string ms(double value) { return std::fabs(value) < 10 ? std::format("{:.1f}", value) : std::format("{:.0f}", value); }

std::function<double()> default_rng() {
    auto engine = std::make_shared<std::mt19937_64>(std::random_device {}());
    return [engine] { return std::uniform_real_distribution<double>(0.0, 1.0)(*engine); };
}

}  // namespace

Delays::Delays(size_t size, std::function<double()> rng)
    : size(size), rng(rng ? std::move(rng) : default_rng()), max(-std::numeric_limits<double>::infinity()) {}

void Delays::add(double value) {
    ++count;
    last = value;
    max = std::max(max, value);
    if (sample.size() < size) {
        sample.push_back(value);
    } else {
        auto slot = static_cast<int64_t>(rng() * static_cast<double>(count));
        if (slot < static_cast<int64_t>(size)) sample[static_cast<size_t>(slot)] = value;
    }
}

std::optional<std::string> Delays::take() {
    if (!count) return std::nullopt;
    std::vector<double> values = sample;
    std::sort(values.begin(), values.end());
    std::string text = std::format("p50 {} / p99 {} / max {} ms (n {})", ms(pct(values, 0.5)), ms(pct(values, 0.99)), ms(max), count);
    count = 0;
    sample.clear();
    max = -std::numeric_limits<double>::infinity();
    return text;
}

LoopWatch::LoopWatch(Logger& log, double tick_s, double stall_ms, double stall_every_s, std::function<void(double)> on_lag)
    : log(log),
      on_lag(std::move(on_lag)),
      tick_s(tick_s),
      stall_ms(stall_ms),
      stall_every_s(stall_every_s),
      stall_logged(-std::numeric_limits<double>::infinity()),
      cpu_time_(process_time()),
      cpu_wall_(monotonic()) {}

awaitable<void> LoopWatch::run() {
    for (;;) {
        double asked = monotonic();
        co_await sleep(tick_s);
        observe((monotonic() - asked - tick_s) * 1000);
    }
}

void LoopWatch::observe(double lag_ms) {
    lag_ms = std::max(lag_ms, 0.0);
    lags.add(lag_ms);
    if (on_lag) on_lag(lag_ms);
    if (lag_ms < stall_ms) return;
    ++stalls;
    ++unlogged;
    double now = monotonic();
    if (now - stall_logged >= stall_every_s) {
        std::string between = unlogged > 1
            ? std::format(" ({} more of {:.0f} ms or longer since the last such line)", unlogged - 1, stall_ms)
            : "";
        log.info("!!! event loop {:.0f} ms late: feeds, guards and cancels all waited that long{}", lag_ms, between);
        stall_logged = now;
        unlogged = 0;
    }
}

std::optional<double> LoopWatch::cpu() {
    double cpu_time = process_time(), wall = monotonic();
    double then_cpu = cpu_time_, then_wall = cpu_wall_;
    cpu_time_ = cpu_time;
    cpu_wall_ = wall;
    if (wall > then_wall) return (cpu_time - then_cpu) / (wall - then_wall);
    return std::nullopt;
}

}  // namespace nmm
