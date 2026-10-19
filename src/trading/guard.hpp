// Price guards: pull the order the moment a price runs away from its recent
// mean (PriceGuard) or a trade prints through the best of the last moments
// (SweepGuard), and open no position right after a sharp move against it
// (TrendGuard, minutes rather than milliseconds). LagGuard is no price guard:
// it pulls both orders while the market data is read late.
//
// Every book update of the watched venue brings a price (a best bid, or a mid)
// and is compared with the mean of that price over the last `window_s` seconds
// before it, weighted by time: each price counts for as long as it stood. A
// price more than `drawdown` (a fraction) under that mean is a drop: it
// threatens a buy, so a working buy is cancelled on the spot and no buy is
// placed for `pause_s`. A price that far over the mean is a rise and does the
// same to sells. Each further update still that far out extends the pause of
// its side; the other side is left alone.
//
// The window is a time span rather than a number of updates because update
// rates differ by venue and by moment: Binance spot sends about 100 updates a
// second, futures several hundred, Arcus a few dozen, and all of them many
// more while the price runs. A window of N updates shrinks to a few
// milliseconds exactly then and its mean follows the move it should catch.
#pragma once

#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/clock.hpp"

namespace nmm {

inline const std::vector<std::string> SIDES {"BUY", "SELL"};

// The Arcus mid a take is scored from: as it stood this long before the take was read (the book update of
// the match can be read a millisecond or two before its trades, 18 Oct logs).
constexpr double TAKE_LOOKBACK_S = 0.05;
constexpr int TAKER_HISTORY = 100;  // a taker is scored on its last this many takes
constexpr int TAKER_ADDRESSES = 5000;  // takers kept at most (the longest unseen goes first)

struct Trip {
    std::string side;  // the side it threatens: BUY on a drop, SELL on a rise
    double mean;  // the mean the price moved away from
    bool fresh;  // it started that side's pause rather than extending it
    bool drop() const { return side == "BUY"; }
};

// The pause of each side a guard keeps: started by a trip, extended by every
// further one, over `pause_s` after the last. (Python's _Pauses.)
class Pauses {
public:
    Pauses(double pause_s, Clock clock);
    virtual ~Pauses();

    bool paused(const std::string& side) const;
    // What a trip on `side` moved away from, as a log line names it.
    virtual std::string reference_of(const std::string& side) const;
    // Forget the window: the book or stream it came from is gone.
    virtual void reset() {}

    std::string reference = "mean";  // what the price moved away from, as a log line names it
    double pause_s;
    double window_s = 0.0;  // 0 for the guards without a window (take, lag)
    Clock clock;
    std::map<std::string, double> until;  // clock time placing each side may resume
    std::map<std::string, int64_t> trips;  // pauses started: by drops (BUY) and rises (SELL)
    std::map<std::string, double> since;  // clock time the latest pause of each side started
    std::map<std::string, int64_t> extended;  // how many further moves extended it
    // the pause the latest fresh trip came after: (since, until, extended), for its log line
    std::map<std::string, std::optional<std::tuple<double, double, int64_t>>> previous;

protected:
    Trip trip(const std::string& side, double mean, double now);
};

class PriceGuard : public Pauses {
public:
    PriceGuard(double window_s, double drawdown, double pause_s, Clock clock = monotonic);

    // Time-weighted mean price over the window ending `now`; nullopt until the
    // prices seen since the last reset cover a whole window.
    std::optional<double> mean(std::optional<double> now = {});
    // Add one price; a Trip when it is too far from the mean before it.
    std::optional<Trip> observe(double price);
    void reset() override;

    double drawdown;
    // (since, price): each price stood from its `since` to the next one's
    std::deque<std::pair<double, double>> steps;

private:
    double area_ = 0.0;  // sum of price * duration over the steps that have ended
    int64_t added_ = 0;
};

// Trades that print through the best of the last `window_s`: a sweep.
//
// Each trade price (Binance spot) is set against the highest and the lowest
// trade of the window before it. More than `drawdown` (a fraction) under the
// high is a drop: a working buy is pulled and no buy is placed until `pause_s`
// after the last trade that far down; that far over the low, the same for
// sells. Against the extreme rather than the mean: a sweep that follows a spike
// up (the bid raised after the spike) trips at once, where the mean, dragged up
// by the spike, is still close.
//
// Overnight 14-15 Oct the takers that filled our quotes did so some 80 ms after
// such a sweep printed on Binance spot, while the book-based guards, on bid
// prices, saw the move 50-80 ms after the trades (median) or not at all: the
// trades come first.
//
// It speaks the language of PriceGuard; the `mean` of a trip is the high (a
// drop) or the low (a rise) the trade went through.
class SweepGuard : public Pauses {
public:
    SweepGuard(double window_s, double drawdown, double pause_s, Clock clock = monotonic);

    // The highest and the lowest trade of the last `window_s`, nullopt without one.
    std::optional<std::pair<double, double>> extremes(std::optional<double> now = {});
    // Add one trade; a Trip when it printed that far through the window's high or low.
    std::optional<Trip> observe(double price);
    void reset() override;

    double drawdown;
    // (time, price) of the trades that can still be the window's high (each newer one
    // lower than the one before) and low (each newer one higher): the first is the extreme
    std::deque<std::pair<double, double>> highs, lows;
};

// Arcus takers come in waves: a taker order that hits one way is often followed within a second by
// more the same way (18 Oct, the day's tape: 29-64% of the top takers' orders came within 500 ms of
// another taker's order the same way, against 4-5% at a random moment). A taker order of `min_usd` or
// more pauses the side it hit (a buy lifts the asks: our sell) until `pause_s` after it; the agent pulls
// the working order there at once. The `mean` of a trip is the taker order's size in dollars.
//
// With `min_takes`, takers the price does not follow pause nothing. Each take is scored by the Arcus
// mid `score_s` after it against the mid just before it, + when it went the taker's way (`mark` feeds
// the mids); a taker with `min_takes` takes scored whose mean is under `min_move` (a fraction) is
// quiet, and its takes neither start nor extend a pause. They are still scored: a quiet taker whose takes
// start to move the price pauses again. 18 Oct 12:19-14:55: 0x38bdc44a sold $813 on BTC every 5-15 s, 879
// takes, +0.02 bp 1 s later, and paused BTC buys 10% of the time; with no quiet taker pausing, the take
// pause of BTC buys would have been 11% of the time rather than 23%, while the informed ones (0x5c82 +0.9
// bp on ETH, 0xd993 +0.6, 0x8c84 +1.1) still pause.
class TakeGuard : public Pauses {
public:
    TakeGuard(double min_usd, double pause_s, Clock clock = monotonic, int min_takes = 0, double min_move = 0.0,
              double score_s = 1.0);

    bool scoring() const { return min_takes > 0; }
    // One taker order: its side (BUY lifts the asks), its size in dollars and its taker's address.
    std::optional<Trip> observe(const std::string& taker_side, double usd, std::optional<std::string> taker = {});
    // The Arcus mid now (nullopt without a book). Scores the takes `score_s` old by now, at the mid that
    // stood then; returns the takers it turned quiet or back: (taker, takes scored, mean, quiet now).
    std::vector<std::tuple<std::string, int, double, bool>> mark(std::optional<double> mid);

    double min_usd;
    int min_takes;
    double min_move;
    double score_s;
    std::set<std::string> quiet;
    int64_t skipped = 0;  // takes of quiet takers: no pause

    struct Pending {
        double read_at;
        int sign;  // +1 buy, -1 sell
        double before;  // the mid before it
        std::string taker;
    };
    std::deque<std::pair<double, std::optional<double>>> mids;  // (since, Arcus mid; nullopt: no book)
    std::deque<Pending> pending;

private:
    std::optional<double> mid_at(double t) const;
    std::optional<std::tuple<std::string, int, double, bool>> score(const std::string& taker, double move);

    // taker -> its latest scores (fractions, + = its way), the longest unseen first
    std::list<std::pair<std::string, std::deque<double>>> moves_;
    std::unordered_map<std::string, std::list<std::pair<std::string, std::deque<double>>>::iterator> index_;
};

// Market data read late: the bot would quote the market as it was, not as it is.
//
// Each reading, how long after its exchange's own time a message was read (or how late the
// event loop ran), of `max_ms` or more pauses both sides until `pause_s` after the last
// such reading; the agent pulls the working order at once. Neither side is safe on an old
// view: a stale fair under the market sells too cheap, one over it buys too dear.
//
// 15 Oct from 13:35 UTC five bots on one core read Binance futures 2-45 s late for four
// hours and kept quoting: 1089 orders refused as crossing the book, -$3.80 in a rally that
// should have paid a long. Before that, on the same host, fills whose newest futures ticker or
// spot trade had been read 200-500 ms late lost 1.8-2.2 bp 5 s later (under 100 ms: none).
//
// It speaks the language of the other guards; the `mean` of a trip is `max_ms`.
class LagGuard : public Pauses {
public:
    LagGuard(double max_ms, double pause_s, Clock clock = monotonic);

    // One reading (ms); both sides' trips when it is `max_ms` or more.
    std::optional<std::pair<Trip, Trip>> observe(double lag_ms);
    // Seconds paused since the start, the running pause up to `now`.
    double paused_for(std::optional<double> now = {});

    double max_ms;
    double worst = 0.0;  // the largest reading of the running (or the last) pause, ms
    double previous_worst = 0.0;  // the same for the pause before it

private:
    double paused_s_ = 0.0;  // pauses that are over, in all
};

// No opening order right after a sharp move against it.
//
// Each price (the futures mid) is set against the highest of the last
// `window_s` seconds when `side` is BUY, the opening side of a long: more than
// `drawdown` (a fraction) under that high is a fall a buy would catch on its way
// down, so a working buy is pulled and no buy is placed until `pause_s` after
// the last price that far down. With `side` SELL (a short), the same for sells
// over the lowest price. The other side, the order that closes the position, is
// never held back: no stop-loss and no taker order, only fewer entries.
// Overnight 14-15 Oct the bots' worst trips were buys into the 02:20 UTC crash
// (ETH -4.2%, ZEC -4.75% in 30 min), several in a row. With `side` "both" (the
// two-sided bot, either side can open a position) it does both: a fall holds
// the buys, a rally the sells.
//
// It speaks the language of PriceGuard (observe returns a Trip, paused, until
// and the rest), so the agent runs both the same way; the `mean` of its trip is
// the high (low) the price came from.
class TrendGuard : public Pauses {
public:
    TrendGuard(double window_s, double drawdown, double pause_s, std::string side = "BUY", Clock clock = monotonic);

    std::string reference_of(const std::string& side) const override;
    // The highest price (BUY) or lowest (SELL) of the last `window_s`.
    std::optional<double> extreme(std::optional<double> now = {}, std::optional<std::string> side = {});
    // Add one price; a Trip when it is that far from the window's extreme. With both
    // sides watched, a price that far from both (a range over twice the drawdown) pauses both;
    // the trip returned is the buy's, and the strategy pulls an order on a paused side.
    std::optional<Trip> observe(double price);
    void reset() override;

    double drawdown;
    std::string side;
    std::vector<std::string> sides;
    // side -> (time, price) of the prices that can still be the window's high (BUY) or low
    // (SELL): each newer one lower (higher) than the one before, so the first is the extreme
    std::map<std::string, std::deque<std::pair<double, double>>> windows;
};

// How far a price (the futures mid) moved over the last `window_s`, and which side that move
// is against: a fall of `threshold` (a fraction) or more is against buys, a rise against sells.
// A side stays "against" for `hold_s` past the last reading that far out, so a move that hovers
// at the threshold does not flip the markup on and off every frame. Each episode is counted and
// timed for the logs. Live 17-18 Oct (BTC 523 fills, ETH 948): fills after the futures mid moved
// 0.2 bp or more against the order within 10 s lost 0.5-0.8 bp in 30 s, the others about nothing.
class DriftWatch {
public:
    DriftWatch(double window_s, double threshold, double hold_s, Clock clock = monotonic);

    // Add one price; the sides whose episode it starts (for their log line).
    std::vector<std::string> observe(double price);
    // The price now over the price `window_s` ago, minus 1; nullopt until the prices seen
    // cover the window.
    std::optional<double> move(std::optional<double> now = {});
    bool against(const std::string& side) const;
    // The move as seen from an order on `side` (a fraction): + in its favour, - against it.
    std::optional<double> for_side(const std::string& side);
    // How long `side` has been "against" in all, the current episode included.
    double seconds(const std::string& side) const;
    // Forget the prices: the book they came from is gone.
    void reset();

    double window_s;
    double threshold;
    double hold_s;
    Clock clock;
    std::deque<std::pair<double, double>> prices;  // (since, price), the first one standing at the window's start
    std::map<std::string, double> until;  // clock time each side stops being "against"
    std::map<std::string, double> since;  // when its current episode started
    std::map<std::string, double> moved;  // the move that started it (a fraction, + = up)
    std::map<std::string, int64_t> episodes;

private:
    std::map<std::string, double> closed_;  // seconds of the episodes that ended
};

}  // namespace nmm
