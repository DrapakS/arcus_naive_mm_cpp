// Venue-agnostic L2 order book with an optional real-time top-of-book overlay.
//
// Prices and sizes are doubles parsed from the exchange's decimal strings: the
// same string always maps to the same double, so prices are safe as map keys.
// Each side is a std::map in ascending price order, so the best bid is the last
// entry and the best ask is the first.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util/json.hpp"

namespace nmm {

using Level = std::pair<double, double>;  // (price, size)

// Best bid and ask as of update id `seq`, from a real-time top-of-book feed.
struct BBO {
    double bid_price = 0, bid_size = 0, ask_price = 0, ask_size = 0;
    int64_t seq = 0;
    int64_t local_ts_us = 0;
};

// Immutable top-N view of a book, safe to hand to strategy code. `seq` is the
// update id the levels reflect: the overlay's while it is newer than the depth
// book. `exchange_ts_us` is from the last depth update.
struct BookTop {
    std::string venue, symbol;
    std::vector<Level> bids;  // best first
    std::vector<Level> asks;  // best first
    int64_t seq = 0;
    std::optional<int64_t> exchange_ts_us;
    int64_t local_ts_us = 0;

    std::optional<double> best_bid() const;
    std::optional<double> best_ask() const;
    std::optional<double> mid() const;
    std::optional<double> spread() const;
};
using BookTopPtr = std::shared_ptr<const BookTop>;

// Local L2 state for one instrument on one venue.
//
// Feeds own the synchronisation logic (snapshots, sequence checks) and call
// apply_snapshot / apply_update / invalidate. Everything else is read-only
// access for the agent.
//
// `max_depth` caps the number of levels kept per side; the worst levels are
// dropped after each change. It bounds memory when a venue streams deltas for
// levels deeper than its snapshot, and is a no-op when it streams a fixed-depth
// view.
//
// apply_bbo layers a real-time best bid/ask (Binance bookTicker) over a depth
// book that arrives in batches. While the overlay's update id is newer than the
// depth book's, reads take each side's best level from it and hide depth levels
// better than it: by that update id they were filled or cancelled. Levels behind
// it come from the depth book, up to one depth batch old. The depth book itself
// is never modified, so its sequence checks are unaffected, and once a depth
// update reaches the overlay's update id it is authoritative again. An overlay
// side with no size is ignored.
//
// The best bid and ask are worked out once per write, and each top(n) is built
// once until the next write: a frame is read many times over (the guards, the
// pricing, the cancel check, the strategy loop), the book only changes when it
// is written.
class OrderBook {
public:
    OrderBook(std::string venue, std::string symbol, std::optional<int> max_depth = std::nullopt);

    // ------------------------------------------------------------- writes
    // Levels as [price, size] (strings or numbers) or {"price": .., "size": ..}.
    void apply_snapshot(const Json& bids, const Json& asks, int64_t seq, std::optional<int64_t> exchange_ts_us = {});
    // Absolute level sizes; a size of zero removes the level.
    void apply_update(const Json& bids, const Json& asks, int64_t seq, std::optional<int64_t> exchange_ts_us = {});
    void apply_snapshot(const std::vector<Level>& bids, const std::vector<Level>& asks, int64_t seq,
                        std::optional<int64_t> exchange_ts_us = {});
    void apply_update(const std::vector<Level>& bids, const std::vector<Level>& asks, int64_t seq,
                      std::optional<int64_t> exchange_ts_us = {});
    // Overlay the best bid/ask as of update id `seq`. Ignored, returning false,
    // unless `seq` is newer than both the depth book and the current overlay.
    bool apply_bbo(double bid_price, double bid_size, double ask_price, double ask_size, int64_t seq);
    // Drop all state; the book is unusable until the next snapshot.
    void invalidate();

    // -------------------------------------------------------------- reads
    // The top-of-book overlay while it is newer than the depth book.
    std::optional<BBO> bbo() const;
    std::optional<Level> best_bid() const { return best_bid_; }
    std::optional<Level> best_ask() const { return best_ask_; }
    std::optional<double> mid() const;
    // Bid levels, best (highest) first; asks best (lowest) first.
    std::vector<Level> bids(std::optional<int> n = {}) const;
    std::vector<Level> asks(std::optional<int> n = {}) const;
    std::pair<size_t, size_t> depth() const { return {bids_.size(), asks_.size()}; }
    bool is_crossed() const;
    // Seconds since the last applied depth snapshot or update. The overlay does
    // not refresh it: a depth feed that stalls while the top-of-book feed keeps
    // flowing still ages out.
    double age() const;
    // True when the book is safe to price from. max_age = 0 skips the age check.
    bool is_ready(double max_age = 0.0) const;
    BookTopPtr top(int n) const;

    std::string venue;
    std::string symbol;
    std::optional<int> max_depth;
    bool synced = false;
    int64_t seq = 0;
    std::optional<int64_t> exchange_ts_us;
    int64_t local_ts_us = 0;
    double updated_mono = 0.0;
    int64_t update_count = 0;
    int64_t bbo_count = 0;

private:
    void set_levels(std::map<double, double>& side, const Json& levels);
    void set_levels(std::map<double, double>& side, const std::vector<Level>& levels);
    void stamp(int64_t seq, std::optional<int64_t> exchange_ts_us);
    void settle();
    void trim();

    std::map<double, double> bids_;
    std::map<double, double> asks_;
    std::optional<BBO> bbo_;
    std::optional<Level> best_bid_;  // as of the last write
    std::optional<Level> best_ask_;
    mutable std::map<int, BookTopPtr> tops_;  // top(n) built since the last write
};

}  // namespace nmm
