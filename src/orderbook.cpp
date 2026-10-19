#include "orderbook.hpp"

#include <limits>
#include <stdexcept>

#include "util/clock.hpp"

namespace nmm {

namespace {

// Accept [price, size] (Binance, Arcus) or {"price", "size"}.
Level level_of(const Json& level) {
    if (level.is_object()) {
        const Json* price = jget(level, "price");
        const Json* size = jget(level, "size");
        if (price == nullptr || size == nullptr) throw std::invalid_argument("level without price or size: " + dump_json(level));
        return {to_float(*price), to_float(*size)};
    }
    const auto& arr = level.as_array();
    if (arr.size() < 2) throw std::invalid_argument("level without price or size: " + dump_json(level));
    return {to_float(arr[0]), to_float(arr[1])};
}

}  // namespace

std::optional<double> BookTop::best_bid() const {
    if (bids.empty()) return std::nullopt;
    return bids.front().first;
}

std::optional<double> BookTop::best_ask() const {
    if (asks.empty()) return std::nullopt;
    return asks.front().first;
}

std::optional<double> BookTop::mid() const {
    if (bids.empty() || asks.empty()) return std::nullopt;
    return (bids.front().first + asks.front().first) / 2.0;
}

std::optional<double> BookTop::spread() const {
    if (bids.empty() || asks.empty()) return std::nullopt;
    return asks.front().first - bids.front().first;
}

OrderBook::OrderBook(std::string venue, std::string symbol, std::optional<int> max_depth)
    : venue(std::move(venue)), symbol(std::move(symbol)), max_depth(max_depth) {}

void OrderBook::apply_snapshot(const Json& bids, const Json& asks, int64_t seq, std::optional<int64_t> exchange_ts_us) {
    bids_.clear();
    asks_.clear();
    set_levels(bids_, bids);
    set_levels(asks_, asks);
    trim();
    synced = true;
    stamp(seq, exchange_ts_us);
    settle();
}

void OrderBook::apply_update(const Json& bids, const Json& asks, int64_t seq, std::optional<int64_t> exchange_ts_us) {
    set_levels(bids_, bids);
    set_levels(asks_, asks);
    trim();
    stamp(seq, exchange_ts_us);
    settle();
}

void OrderBook::apply_snapshot(const std::vector<Level>& bids, const std::vector<Level>& asks, int64_t seq,
                               std::optional<int64_t> exchange_ts_us) {
    bids_.clear();
    asks_.clear();
    set_levels(bids_, bids);
    set_levels(asks_, asks);
    trim();
    synced = true;
    stamp(seq, exchange_ts_us);
    settle();
}

void OrderBook::apply_update(const std::vector<Level>& bids, const std::vector<Level>& asks, int64_t seq,
                             std::optional<int64_t> exchange_ts_us) {
    set_levels(bids_, bids);
    set_levels(asks_, asks);
    trim();
    stamp(seq, exchange_ts_us);
    settle();
}

bool OrderBook::apply_bbo(double bid_price, double bid_size, double ask_price, double ask_size, int64_t seq) {
    if (seq <= this->seq || (bbo_ && seq <= bbo_->seq)) return false;
    bbo_ = BBO {bid_price, bid_size, ask_price, ask_size, seq, now_us()};
    ++bbo_count;
    settle();
    return true;
}

void OrderBook::invalidate() {
    bids_.clear();
    asks_.clear();
    bbo_.reset();
    synced = false;
    seq = 0;
    exchange_ts_us.reset();
    settle();
}

void OrderBook::stamp(int64_t seq, std::optional<int64_t> exchange_ts_us) {
    this->seq = seq;
    this->exchange_ts_us = exchange_ts_us;
    local_ts_us = now_us();
    updated_mono = monotonic();
    ++update_count;
}

void OrderBook::settle() {
    auto overlay = bbo();
    if (overlay && overlay->bid_size > 0.0) best_bid_ = Level {overlay->bid_price, overlay->bid_size};
    else if (!bids_.empty()) best_bid_ = *bids_.rbegin();
    else best_bid_.reset();
    if (overlay && overlay->ask_size > 0.0) best_ask_ = Level {overlay->ask_price, overlay->ask_size};
    else if (!asks_.empty()) best_ask_ = *asks_.begin();
    else best_ask_.reset();
    if (!tops_.empty()) tops_.clear();
}

void OrderBook::trim() {
    if (!max_depth) return;
    size_t depth = static_cast<size_t>(std::max(*max_depth, 0));
    while (bids_.size() > depth) bids_.erase(bids_.begin());
    while (asks_.size() > depth) asks_.erase(std::prev(asks_.end()));
}

void OrderBook::set_levels(std::map<double, double>& side, const Json& levels) {
    if (levels.is_null()) return;
    for (const auto& level : levels.as_array()) {
        auto [price, size] = level_of(level);
        if (size > 0.0) side[price] = size;
        else side.erase(price);
    }
}

void OrderBook::set_levels(std::map<double, double>& side, const std::vector<Level>& levels) {
    for (const auto& [price, size] : levels) {
        if (size > 0.0) side[price] = size;
        else side.erase(price);
    }
}

std::optional<BBO> OrderBook::bbo() const {
    if (bbo_ && bbo_->seq > seq) return bbo_;
    return std::nullopt;
}

std::optional<double> OrderBook::mid() const {
    if (!best_bid_ || !best_ask_) return std::nullopt;
    return (best_bid_->first + best_ask_->first) / 2.0;
}

std::vector<Level> OrderBook::bids(std::optional<int> n) const {
    std::vector<Level> out;
    if (n && *n <= 0) return out;
    size_t limit = n ? static_cast<size_t>(*n) : std::numeric_limits<size_t>::max();
    auto overlay = bbo();
    if (!overlay || overlay->bid_size <= 0.0) {
        for (auto it = bids_.rbegin(); it != bids_.rend() && out.size() < limit; ++it) out.emplace_back(*it);
        return out;
    }
    // the overlay on top, then depth levels strictly below the overlay's bid
    out.emplace_back(overlay->bid_price, overlay->bid_size);
    auto end = bids_.lower_bound(overlay->bid_price);
    for (auto it = std::make_reverse_iterator(end); it != bids_.rend() && out.size() < limit; ++it) out.emplace_back(*it);
    return out;
}

std::vector<Level> OrderBook::asks(std::optional<int> n) const {
    std::vector<Level> out;
    if (n && *n <= 0) return out;
    size_t limit = n ? static_cast<size_t>(*n) : std::numeric_limits<size_t>::max();
    auto overlay = bbo();
    if (!overlay || overlay->ask_size <= 0.0) {
        for (auto it = asks_.begin(); it != asks_.end() && out.size() < limit; ++it) out.emplace_back(*it);
        return out;
    }
    // the overlay's ask, then depth levels strictly above it
    out.emplace_back(overlay->ask_price, overlay->ask_size);
    for (auto it = asks_.upper_bound(overlay->ask_price); it != asks_.end() && out.size() < limit; ++it) out.emplace_back(*it);
    return out;
}

bool OrderBook::is_crossed() const { return best_bid_ && best_ask_ && best_bid_->first >= best_ask_->first; }

double OrderBook::age() const {
    if (updated_mono == 0.0) return std::numeric_limits<double>::infinity();
    return monotonic() - updated_mono;
}

bool OrderBook::is_ready(double max_age) const {
    return synced && best_bid_ && best_ask_ && best_bid_->first < best_ask_->first && (max_age == 0.0 || age() <= max_age);
}

BookTopPtr OrderBook::top(int n) const {
    auto it = tops_.find(n);
    if (it != tops_.end()) return it->second;
    // the first call since the last write: built once, then shared (it is immutable)
    auto overlay = bbo();
    auto built = std::make_shared<BookTop>();
    built->venue = venue;
    built->symbol = symbol;
    built->bids = bids(n);
    built->asks = asks(n);
    built->seq = overlay ? overlay->seq : seq;
    built->exchange_ts_us = exchange_ts_us;
    built->local_ts_us = overlay ? overlay->local_ts_us : local_ts_us;
    BookTopPtr top = built;
    tops_.emplace(n, top);
    return top;
}

}  // namespace nmm
