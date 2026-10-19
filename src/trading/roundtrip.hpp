// Round trips: from flat to flat, one log line each.
//
// Every fill of ours comes in through `fill` (a maker fills at its own price),
// and while a position is open the Arcus top comes in through `mark`: a long is
// worth the best bid, a short the best ask. When the position is back at zero,
// one line sums the trip up: the average buy and sell, how long it was held, the
// result in bp of what was bought and in dollars, and the worst point on the
// way (the trip's mark-to-market at the moment it stood lowest).
//
// The line also tells how the trip was closed: how many orders the exit took
// (`placed`: the first one and every re-place), how far the first one stood
// from the Arcus mid, and for how much of the time an exit order worked it was
// at the best price of its side (`mark` gets the working order too).
//
// Only the fills of our orders count. A position the bot did not build itself
// (found at start, or a leftover order filled) is not a trip: `sync` sets it
// aside, with the exchange's average entry price when known. Fills that reduce
// it work it off first, and once it is gone one line says what it made; fills
// that add to the position open or extend a trip of our own, so a long found at
// start that is too small to sell alone does not swallow the trip that sells
// it. A trip left with less than `dust` (the market's minimum size, which
// cannot be sold on its own) is closed, the rest set aside.
#pragma once

#include <optional>
#include <string>
#include <utility>

#include "util/clock.hpp"
#include "util/decimal.hpp"
#include "util/log.hpp"

namespace nmm {

struct RoundTrip {
    std::string side;  // the side that opened it: BUY = long, SELL = short
    double opened = 0.0;  // clock time of the first fill
    Decimal bought;
    Decimal bought_value;
    Decimal sold;
    Decimal sold_value;
    int fills = 0;
    double worst_bps = 0.0;  // lowest mark-to-market seen, bp of the opening side's value
    double worst_after = 0.0;  // seconds after opening
    int exits = 0;  // exit orders placed (the first and every re-place)
    std::optional<double> first_exit_bps;  // how far the first one stood from the mid, away from the market
    double exit_s = 0.0;  // time an exit order worked, as seen by mark
    double at_best_s = 0.0;  // of which at the best price of its side
    std::optional<double> last_mark;
    bool last_exit = false;  // at the last mark: an exit order worked
    bool last_at_best = false;  // ... and it was at the best price
    std::optional<std::pair<double, double>> last_top;  // the Arcus bid/ask at the last mark

    Decimal position() const { return bought - sold; }
    Decimal entry_value() const { return side == "BUY" ? bought_value : sold_value; }
    double value_bps(const Decimal& cash) const;
};

class RoundTrips {
public:
    RoundTrips(Logger& log, Clock clock = monotonic, Decimal dust = 0, int places = 2);

    bool open() const { return trip.has_value(); }
    Decimal position() const { return untracked + (trip ? trip->position() : Decimal(0)); }

    // A fill of ours; `working`: its order still works the rest of its size.
    void fill(const std::string& side, Decimal size, Decimal price, bool working = false);
    // An order of ours on `side` is done: a trip it left with a crumb ends. Only a trip
    // the exit side has worked on: a partial opening fill under the minimum size, closed
    // together with a set-aside position, is no crumb of an exit (it made a zero-size trip
    // and handed its fill to the set-aside).
    void settle(const std::string& side);
    // An order went out: on the exit side of an open trip, it counts.
    void placed(const std::string& side, Decimal price, std::optional<double> mid);
    // The Arcus top while a trip is open, with our working order: the worst point of the
    // trip and how long an exit order stood at the best price.
    void mark(double bid, double ask, std::optional<std::string> order_side = {}, std::optional<double> order_price = {});
    // The exchange's position (and its average entry price, when known): what our fills
    // do not explain is set aside.
    void sync(Decimal position, std::optional<Decimal> entry = {});
    // At shutdown: what is still open and what it is worth at `bid`/`ask` or, without
    // them, at the Arcus top last seen with the trip open. (Python's final().)
    void report_final(std::optional<double> bid = {}, std::optional<double> ask = {});
    std::string status() const;

    Logger* log;
    Clock clock;
    Decimal dust;
    int places;  // decimals prices are shown with: one more than the market's tick has
    std::optional<RoundTrip> trip;
    Decimal untracked;  // a position not built by our fills, set aside
    std::optional<Decimal> aside_entry;  // its average entry price, when the exchange gave one
    int count = 0;
    Decimal pnl;  // dollars, all trips closed so far
    Decimal turnover;  // dollars bought and sold in those trips, opening side only

private:
    void end(RoundTrip& t);
    void aside_closed(const std::string& side);
    void close(RoundTrip t, Decimal left = 0);
    std::string line(const RoundTrip& t, const Decimal& cash, double total_bps, const Decimal& left) const;
    std::string avg(const Decimal& value, const Decimal& size) const;

    Decimal aside_size_;  // what fills worked off of it so far, and at what value
    Decimal aside_value_;
};

}  // namespace nmm
