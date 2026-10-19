#include "trading/guard.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

#include "pricing.hpp"

namespace nmm {

Pauses::Pauses(double pause_s, Clock clock) : pause_s(pause_s), clock(std::move(clock)) {
    for (const auto& side : SIDES) {
        until[side] = 0.0;
        trips[side] = 0;
        since[side] = 0.0;
        extended[side] = 0;
        previous[side] = std::nullopt;
    }
}

Pauses::~Pauses() = default;

bool Pauses::paused(const std::string& side) const { return clock() < until.at(side); }

std::string Pauses::reference_of(const std::string&) const { return reference; }

Trip Pauses::trip(const std::string& side, double mean, double now) {
    bool fresh = now >= until[side];
    if (fresh) {
        trips[side] += 1;
        if (until[side] != 0.0) previous[side] = std::make_tuple(since[side], until[side], extended[side]);
        since[side] = now;
        extended[side] = 0;
    } else {
        extended[side] += 1;
    }
    until[side] = now + pause_s;
    return Trip {side, mean, fresh};
}

// ------------------------------------------------------------------ PriceGuard

PriceGuard::PriceGuard(double window_s_, double drawdown, double pause_s, Clock clock)
    : Pauses(pause_s, std::move(clock)), drawdown(drawdown) {
    if (!(window_s_ > 0) || !(0 < drawdown && drawdown < 1) || pause_s < 0)
        throw std::invalid_argument("PriceGuard needs window_s > 0, 0 < drawdown < 1 and pause_s >= 0");
    window_s = window_s_;
}

std::optional<double> PriceGuard::mean(std::optional<double> now_) {
    double now = now_ ? *now_ : clock();
    double start = now - window_s;
    if (steps.empty() || steps.front().first > start) return std::nullopt;
    while (steps.size() > 1 && steps[1].first <= start) {  // ended before the window
        auto [since_, price] = steps.front();
        steps.pop_front();
        area_ -= price * (steps.front().first - since_);
    }
    auto [first_since, first] = steps.front();
    auto [last_since, last] = steps.back();
    double area = area_ - first * (start - first_since) + last * (now - last_since);
    return area / window_s;
}

std::optional<Trip> PriceGuard::observe(double price) {
    double now = clock();
    std::optional<Trip> tripped;
    if (auto m = mean(now)) {
        if (price < *m * (1.0 - drawdown)) tripped = trip("BUY", *m, now);
        else if (price > *m * (1.0 + drawdown)) tripped = trip("SELL", *m, now);
    }
    if (!steps.empty()) {
        auto [since_, last] = steps.back();
        if (price == last) return tripped;  // the same price stands on
        area_ += last * (now - since_);
    }
    steps.emplace_back(now, price);
    added_ += 1;
    if (added_ % 1000 == 0) {  // no float drift over a long run
        std::vector<double> parts;
        for (size_t i = 0; i + 1 < steps.size(); ++i) parts.push_back(steps[i].second * (steps[i + 1].first - steps[i].first));
        area_ = fsum(parts);
    }
    return tripped;
}

void PriceGuard::reset() {
    steps.clear();
    area_ = 0.0;
}

// ------------------------------------------------------------------ SweepGuard

SweepGuard::SweepGuard(double window_s_, double drawdown, double pause_s, Clock clock)
    : Pauses(pause_s, std::move(clock)), drawdown(drawdown) {
    if (!(window_s_ > 0) || !(0 < drawdown && drawdown < 1) || pause_s < 0)
        throw std::invalid_argument("SweepGuard needs window_s > 0, 0 < drawdown < 1 and pause_s >= 0");
    window_s = window_s_;
}

std::optional<std::pair<double, double>> SweepGuard::extremes(std::optional<double> now_) {
    double now = now_ ? *now_ : clock();
    double start = now - window_s;
    for (auto* window : {&highs, &lows})
        while (!window->empty() && window->front().first < start) window->pop_front();
    if (highs.empty()) return std::nullopt;
    return std::make_pair(highs.front().second, lows.front().second);
}

std::optional<Trip> SweepGuard::observe(double price) {
    double now = clock();
    std::optional<Trip> tripped;
    if (auto ext = extremes(now)) {
        auto [high, low] = *ext;
        if (price < high * (1.0 - drawdown)) tripped = trip("BUY", high, now);
        else if (price > low * (1.0 + drawdown)) tripped = trip("SELL", low, now);
    }
    while (!highs.empty() && highs.back().second <= price) highs.pop_back();
    highs.emplace_back(now, price);
    while (!lows.empty() && lows.back().second >= price) lows.pop_back();
    lows.emplace_back(now, price);
    return tripped;
}

void SweepGuard::reset() {
    highs.clear();
    lows.clear();
}

// ------------------------------------------------------------------- TakeGuard

TakeGuard::TakeGuard(double min_usd, double pause_s, Clock clock, int min_takes, double min_move, double score_s)
    : Pauses(pause_s, std::move(clock)), min_usd(min_usd), min_takes(min_takes), min_move(min_move), score_s(score_s) {
    if (min_usd < 0 || !(pause_s > 0)) throw std::invalid_argument("TakeGuard needs min_usd >= 0 and pause_s > 0");
    if (!(0 <= min_takes && min_takes <= TAKER_HISTORY) || min_move < 0 || !(score_s > 0))
        throw std::invalid_argument(std::format(
            "TakeGuard needs min_takes within 0..{}, min_move >= 0 and score_s > 0", TAKER_HISTORY));
    reference = "take";
}

std::optional<Trip> TakeGuard::observe(const std::string& taker_side, double usd, std::optional<std::string> taker) {
    if (usd < min_usd) return std::nullopt;
    double now = clock();
    if (min_takes && taker && !taker->empty()) {
        if (auto before = mid_at(now - TAKE_LOOKBACK_S)) pending.push_back({now, taker_side == "BUY" ? 1 : -1, *before, *taker});
        if (quiet.count(*taker)) {
            skipped += 1;
            return std::nullopt;
        }
    }
    return trip(taker_side == "BUY" ? "SELL" : "BUY", usd, now);
}

std::vector<std::tuple<std::string, int, double, bool>> TakeGuard::mark(std::optional<double> mid) {
    double now = clock();
    std::vector<std::tuple<std::string, int, double, bool>> flips;
    while (!pending.empty() && pending.front().read_at + score_s <= now) {
        Pending p = pending.front();
        pending.pop_front();
        std::optional<double> then = mids.empty() ? std::nullopt : mids.back().second;  // the mid before this one stood until now
        if (then) {
            if (auto flip = score(p.taker, p.sign * (*then / p.before - 1.0))) flips.push_back(*flip);
        }
    }
    if (mids.empty() || mids.back().second != mid) mids.emplace_back(now, mid);
    while (mids.size() > 1 && mids[1].first <= now - TAKE_LOOKBACK_S) mids.pop_front();
    return flips;
}

std::optional<double> TakeGuard::mid_at(double t) const {
    for (auto it = mids.rbegin(); it != mids.rend(); ++it)
        if (it->first <= t) return it->second;
    return std::nullopt;
}

std::optional<std::tuple<std::string, int, double, bool>> TakeGuard::score(const std::string& taker, double move) {
    std::deque<double> history;
    if (auto it = index_.find(taker); it != index_.end()) {  // re-inserted: the list runs from the longest unseen taker
        history = std::move(it->second->second);
        moves_.erase(it->second);
        index_.erase(it);
    } else if (static_cast<int>(moves_.size()) >= TAKER_ADDRESSES) {
        std::string oldest = moves_.front().first;
        index_.erase(oldest);
        moves_.pop_front();
        quiet.erase(oldest);
    }
    history.push_back(move);
    if (static_cast<int>(history.size()) > TAKER_HISTORY) history.pop_front();
    moves_.emplace_back(taker, std::move(history));
    index_[taker] = std::prev(moves_.end());
    const auto& moves = moves_.back().second;
    if (static_cast<int>(moves.size()) < min_takes) return std::nullopt;
    double sum = 0.0;
    for (double m : moves) sum += m;
    double mean = sum / static_cast<double>(moves.size());
    bool is_quiet = mean < min_move;
    if (is_quiet == (quiet.count(taker) > 0)) return std::nullopt;
    if (is_quiet) quiet.insert(taker);
    else quiet.erase(taker);
    return std::make_tuple(taker, static_cast<int>(moves.size()), mean, is_quiet);
}

// -------------------------------------------------------------------- LagGuard

LagGuard::LagGuard(double max_ms, double pause_s, Clock clock) : Pauses(pause_s, std::move(clock)), max_ms(max_ms) {
    if (!(max_ms > 0) || pause_s < 0) throw std::invalid_argument("LagGuard needs max_ms > 0 and pause_s >= 0");
    reference = "limit";
}

std::optional<std::pair<Trip, Trip>> LagGuard::observe(double lag_ms) {
    if (lag_ms < max_ms) return std::nullopt;
    double now = clock();
    if (now >= until["BUY"]) {  // a new pause
        if (trips["BUY"]) paused_s_ += until["BUY"] - since["BUY"];
        previous_worst = worst;
        worst = 0.0;
    }
    worst = std::max(worst, lag_ms);
    Trip buy = trip("BUY", max_ms, now);
    Trip sell = trip("SELL", max_ms, now);
    return std::make_pair(buy, sell);
}

double LagGuard::paused_for(std::optional<double> now_) {
    double now = now_ ? *now_ : clock();
    double running = trips["BUY"] ? std::min(now, until["BUY"]) - since["BUY"] : 0.0;
    return paused_s_ + std::max(running, 0.0);
}

// ------------------------------------------------------------------ TrendGuard

TrendGuard::TrendGuard(double window_s_, double drawdown, double pause_s, std::string side_, Clock clock)
    : Pauses(pause_s, std::move(clock)), drawdown(drawdown), side(std::move(side_)) {
    if (!(window_s_ > 0) || !(0 < drawdown && drawdown < 1) || pause_s < 0
        || (side != "BUY" && side != "SELL" && side != "both"))
        throw std::invalid_argument(
            "TrendGuard needs window_s > 0, 0 < drawdown < 1, pause_s >= 0 and side BUY, SELL or \"both\"");
    window_s = window_s_;
    sides = side == "both" ? SIDES : std::vector<std::string> {side};
    reference = side == "BUY" ? "high" : side == "SELL" ? "low" : "high or low";
    for (const auto& s : sides) windows[s];
}

std::string TrendGuard::reference_of(const std::string& s) const { return s == "BUY" ? "high" : "low"; }

std::optional<double> TrendGuard::extreme(std::optional<double> now_, std::optional<std::string> side_) {
    double now = now_ ? *now_ : clock();
    auto& window = windows.at(side_ ? *side_ : sides[0]);
    while (!window.empty() && window.front().first < now - window_s) window.pop_front();
    if (window.empty()) return std::nullopt;
    return window.front().second;
}

std::optional<Trip> TrendGuard::observe(double price) {
    double now = clock();
    std::vector<Trip> trips;
    for (const auto& s : sides) {
        auto ext = extreme(now, s);
        if (ext && (s == "BUY" ? price < *ext * (1.0 - drawdown) : price > *ext * (1.0 + drawdown)))
            trips.push_back(trip(s, *ext, now));
        auto& window = windows[s];
        if (s == "BUY") {
            while (!window.empty() && window.back().second <= price) window.pop_back();
        } else {
            while (!window.empty() && window.back().second >= price) window.pop_back();
        }
        window.emplace_back(now, price);
    }
    tripped = trips;
    if (trips.empty()) return std::nullopt;
    return trips.front();
}

void TrendGuard::reset() {
    for (auto& [_, window] : windows) window.clear();
}

// ------------------------------------------------------------------ DriftWatch

DriftWatch::DriftWatch(double window_s, double threshold, double hold_s, Clock clock)
    : window_s(window_s), threshold(threshold), hold_s(hold_s), clock(std::move(clock)) {
    if (!(0 < window_s && window_s <= 600) || !(0 < threshold && threshold < 0.01) || !(0 <= hold_s && hold_s <= 600))
        throw std::invalid_argument("DriftWatch needs 0 < window_s <= 600, 0 < threshold < 0.01 and 0 <= hold_s <= 600");
    for (const auto& side : SIDES) {
        until[side] = 0.0;
        since[side] = 0.0;
        moved[side] = 0.0;
        episodes[side] = 0;
        closed_[side] = 0.0;
    }
}

std::vector<std::string> DriftWatch::observe(double price) {
    double now = clock();
    if (prices.empty() || prices.back().second != price) prices.emplace_back(now, price);
    double start = now - window_s;
    while (prices.size() > 1 && prices[1].first <= start) prices.pop_front();
    auto m = move(now);
    std::vector<std::string> started;
    if (m) {
        for (const auto& [side, is_against] : {std::pair<std::string, bool> {"BUY", *m <= -threshold},
                                               std::pair<std::string, bool> {"SELL", *m >= threshold}}) {
            if (!is_against) continue;
            if (now >= until[side]) {  // a new episode
                if (until[side] != 0.0) closed_[side] += until[side] - since[side];
                episodes[side] += 1;
                since[side] = now;
                moved[side] = *m;
                started.push_back(side);
            }
            until[side] = now + hold_s;
        }
    }
    return started;
}

std::optional<double> DriftWatch::move(std::optional<double> now_) {
    double now = now_ ? *now_ : clock();
    if (prices.empty() || prices.front().first > now - window_s) return std::nullopt;
    return prices.back().second / prices.front().second - 1.0;
}

bool DriftWatch::against(const std::string& side) const { return clock() < until.at(side); }

std::optional<double> DriftWatch::for_side(const std::string& side) {
    auto m = move();
    if (!m) return std::nullopt;
    return side == "BUY" ? *m : -*m;
}

double DriftWatch::seconds(const std::string& side) const {
    double now = clock();
    double u = until.at(side);
    double current = u != 0.0 ? std::min(now, u) - since.at(side) : 0.0;
    return closed_.at(side) + current;
}

void DriftWatch::reset() { prices.clear(); }

}  // namespace nmm
