// Naive quoting: ALO limit orders, open first, then close what was opened; or both sides at once.
//
// direction "long" buys first and sells what was bought (the default); "short"
// mirrors it: sells first and buys back what was sold. Below, "open" is the buy of
// a long (the sell of a short) and "close" the other side. Both work one order at
// a time.
//
// direction "both" quotes a buy and a sell together, order_size each, and lets
// the position go either way up to max_position_usd (dollars at the order's
// price): each side's order is cut to what keeps the position within it, and a
// side with no room left for a valid order is not quoted (its working order is
// pulled) until the position on that side is under max_position_resume of the
// limit again. Whichever order would shrink the position is its closing order: it
// takes the cut markup (close_markup_cut) and close_tol_away; flat, both sides open.
//
// Inventory controls for both sides, all maker only (no taker order, no stop): the
// closing order is the bigger of the whole position and order_size; the position
// as a share u of the limit skews the markups (opening side x (1 + skew_open u),
// closing side x (1 - skew_close u)); a position held exit_after_s or exit_loss
// under water at the Arcus mid gets its closing order at the best price on its
// side, joined and never crossed, with no markup; and while add_guard holds a side
// after a futures move against it, an order there may take the position on that
// side to add_limit_share of the limit at most.
//
// Side and size come from the position. Flat, or in the direction by less than one
// valid order: open order_size, unless that is itself under the minimum notional
// (then nothing is quoted, and an error says so). In the direction by at least one
// valid order: close the whole position. A position against the direction (left
// by a run in the other one) is taken back to zero first.
//
// Prices, with `tick` the market's price step at that price:
//
//     buy  = min(fair_bid * (1 - markup), best_ask - tick), rounded down to the tick
//     sell = max(best_bid + tick, (fair_bid + tick) * (1 + markup * (1 - close_markup_cut))), rounded up
//
// A working order is cancelled when its price is off the current target by more
// than tol (as a fraction of the target), when it is on the wrong side, or when
// there is no price; the next evaluation then places the replacement. tol_away
// applies instead when the target moved away from the order, leaving it deeper in
// the book; close_tol_away for the closing order.
//
// With price guards, a sharp move pulls the order it threatens whatever its price
// (pull): a drop pulls a buy, a rise a sell. No order is placed on a side while
// any guard pauses that side. With an Imbalance, a lopsided Arcus book widens the
// quote on its thin side. never_lead keeps the order among the others on its side
// of the book. With an open_guard, around each open of the underlying no order
// opens or adds to the position. With a session (a market with trading hours),
// outside it every order takes off_hours_markup_factor times its markup.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "orderbook.hpp"
#include "trading/gateway.hpp"
#include "trading/guard.hpp"
#include "trading/market.hpp"
#include "trading/opens.hpp"
#include "trading/session.hpp"
#include "util/clock.hpp"
#include "util/decimal.hpp"
#include "view.hpp"

namespace nmm {

// After the book came to our order on a side, no new order there for this long: the market is
// moving onto that price (run 14: both rejects followed such a cancel within 1 ms).
constexpr double REPLACE_AFTER_CAME_S = 0.03;

// Best price on one side of the book (levels, best first) without our own order of `size` at
// `price`: a level no bigger than it is only us.
std::optional<double> others_best(const std::vector<Level>& levels, std::optional<double> price = {}, double size = 0.0);

// Where a new order at `price` joins the book on `side` (`levels`: the others' visible levels,
// best first): ticks behind their best, the size already resting at its price, and the size at
// better prices in the visible levels.
std::string queue_note(const std::vector<Level>& levels, const std::string& side, double price, double tick);

// How far the futures fair is from the spot fair, seen from an order on `side`.
std::optional<std::string> gap_note(std::optional<double> gap, std::optional<double> change, const std::string& side);

// The futures move over the drift window seen from an order on `side` ("drift -0.35 bp in 10 s
// for it"), and ", markup x1.5" when it widens that side; none until the window is covered.
// `factor` is shown as Python's str(Decimal(str(factor))).
std::optional<std::string> drift_text(DriftWatch& drift, const std::string& side, double factor);

// thin_side in words: "bids 6.2x asks" when the asks are thin; with `held`, "held: bids were 6.2x asks".
std::string lopsided(const std::pair<std::string, double>& thin, bool held = false);

// A lopsided Arcus book: on the first `levels` levels one side has more than `ratio` times the
// volume of the other; an order on the thin side gets `factor` times the markup, for `hold_s`
// after the book was last that lopsided. `sides`: "open" (the opening order) or "both".
struct Imbalance {
    int levels = 5;
    double ratio = 5.0;
    double factor = 5.0;
    double hold_s = 0.0;
    std::string sides = "open";
};

struct Quote {
    std::string side;
    Decimal price;
    Decimal size;
    bool operator==(const Quote&) const = default;
};

struct StrategyOptions {
    double markup = 0.0;
    double tol = 0.0;
    double order_size = 0.0;
    std::vector<Pauses*> guards;
    bool never_lead = false;
    std::optional<Imbalance> imbalance;
    double tol_away = 0.0;
    double close_markup_cut = 0.0;
    double close_tol_away = 0.0;
    std::string direction = "long";
    double max_position_usd = 1000.0;
    double max_position_resume = 0.95;
    SessionClock* session = nullptr;
    double off_hours_markup_factor = 1.0;
    double skew_open = 0.0;
    double skew_close = 0.0;
    double exit_after_s = 0.0;
    double exit_loss = 0.0;
    TrendGuard* add_guard = nullptr;
    double add_limit_share = 0.5;
    DriftWatch* drift = nullptr;
    double drift_factor = 1.0;
    double came_hold_s = 0.0;
    OpenWindows* open_guard = nullptr;
    double open_limit_share = 0.0;
};

class QuoteStrategy {
public:
    // `gateway` null: it only prices (mode quote). Throws std::invalid_argument on bad options.
    QuoteStrategy(Gateway* gateway, MarketSpec market, StrategyOptions options);

    // An order on `side` is the closing one: the fixed close side, or with both sides quoted, the
    // one that shrinks the position (none while flat).
    bool closing(const std::string& side, std::optional<Decimal> position = {}) const;
    // The position is the other direction's (never with both sides).
    bool against(const Decimal& position) const;
    // An order on `side` takes the thin-side markup when that side is thin.
    bool widens(const std::string& side) const;
    // Which side to quote (one side at a time).
    std::string side(const Decimal& position, const MarketView* view = nullptr);
    // The order we want right now, or none when we cannot price one.
    std::optional<Quote> target(const MarketView& view, const Decimal& position);
    Decimal size(const std::string& side, const Decimal& position, const Decimal& price) const;
    // Where an order on `side` would go now, or none without a price.
    std::optional<Decimal> price(const std::string& side, const MarketView& view);
    // The price the book last came to our order at on `side` while came_hold_s holds it.
    std::optional<Decimal> came_hold(const std::string& side) const;
    // The futures mid moved against `side` lately, and that widens its markup.
    bool drifted(const std::string& side) const;
    std::optional<std::string> drift_note(const std::string& side) const;
    // Both sides: how much of max_position_usd the position takes at `price`, 0..1.
    Decimal used(const Decimal& price) const;
    // The position has been held exit_after_s or is exit_loss under water: its closing order goes
    // to the best price. Logged when it starts and ends.
    bool urgent(const MarketView& view);
    // The side of the Arcus book that is thin on its first levels (BUY: bids, SELL: asks) and how
    // many times more the other side has, or none while the book is not lopsided.
    std::optional<std::pair<std::string, double>> thin_side(const MarketView& view);
    // (price, size) of our order on `side` that the public book can be showing.
    std::optional<std::pair<double, double>> own_in_book(const std::string& side, const std::vector<Level>* levels = nullptr);
    // `levels` with our own order taken out.
    std::vector<Level> others_levels(const std::vector<Level>& levels, const std::string& side);
    // The others' best price on `side` of the Arcus book, our own order taken out.
    std::optional<double> best_of_others(const MarketView& view, const std::string& side);
    void evaluate(const MarketView& view);
    // Both sides: the order wanted on each of `sides`, or none (no price, or no room under the limit).
    std::map<std::string, std::optional<Quote>> targets(const MarketView& view, const Decimal& position,
                                                        const std::vector<std::string>& sides = {"BUY", "SELL"});
    // Where an order on each side would go now: what the last targets() of both sides found for
    // this very view (the strategy loop's), else priced now.
    std::map<std::string, std::optional<Decimal>> prices(const MarketView& view);
    // Both sides: order_size cut to what keeps the position within max_position_usd at `price`;
    // none when no valid order fits or order_size itself is under the minimum notional.
    std::optional<Decimal> both_size(const std::string& side, const Decimal& position, const Decimal& price);
    // Within a market-open window of open_guard.
    bool at_open() const;
    // The cancel half of evaluate alone, cheap enough to run on every book frame.
    void check_cancel(const MarketView& view);
    // A guard holds off placing on `side`.
    bool paused(const std::string& side) const;
    // Cancel the working order on `side` now, whatever its price: the guard tripped.
    void pull(const std::string& side, const std::string& why);
    // An order on `side` at `price` is too far from `target`.
    bool needs_move(const std::string& side, const Decimal& price, const Decimal& target, bool strict = false) const;

    std::string direction;
    bool both;
    std::string open_side, close_side;
    Decimal max_position_usd, max_position_resume;
    SessionClock* session;
    Decimal off_hours_factor;
    Decimal skew_open, skew_close;
    double exit_after_s;
    Decimal exit_loss;
    TrendGuard* add_guard;
    Decimal add_limit_share;
    OpenWindows* open_guard;
    Decimal open_limit_share;
    DriftWatch* drift;
    Decimal drift_factor;
    double drift_factor_float;
    Gateway* gateway;
    MarketSpec market;
    std::vector<Pauses*> guards;
    bool never_lead;
    std::optional<Imbalance> imbalance;
    double came_hold_s;
    Clock clock = monotonic;
    bool thin_held = false;  // the latest thin_side answer comes from the hold, not from the book now
    Decimal markup, close_markup, tol, tol_away, close_tol_away, order_size;
    std::map<std::string, bool> limited_ {{"BUY", false}, {"SELL", false}};  // both: a side held at the position limit
    std::map<std::string, bool> add_held_ {{"BUY", false}, {"SELL", false}};

private:
    friend struct QuoteStrategyTestAccess;

    Decimal position_now() const;
    bool big_enough(const Decimal& size, const std::string& side, const MarketView* view);
    void evaluate_both(const MarketView& view);
    std::optional<Decimal> open_cap(const std::string& side, const Decimal& size, const Decimal& position, const Decimal& price) const;
    Decimal closing_size(const Decimal& position, const Decimal& room) const;
    std::optional<Decimal> add_cap(const std::string& side, const Decimal& size, const Decimal& position, const Decimal& price);
    void place(const Quote& quote, const MarketView& view);
    // Why the working order has to go, or none while it is still the right one.
    std::optional<std::string> cancel_reason(const OrderPtr& order, const std::optional<Quote>& quote, const MarketView& view);
    std::optional<std::string> book_came(const OrderPtr& order, const MarketView& view);
    double volume(const std::vector<Level>& levels, const std::string& side);

    Decimal size_slack_;
    std::optional<double> urgent_;  // position_since of the position whose exit is urgent now (logged once)
    // side -> the order placed there, the others' best when it was placed, and whether it was placed
    // under the thin-side markup
    std::map<std::string, std::tuple<OrderPtr, std::optional<double>, bool>> placed_;
    // side -> the order the book last came to there, and when it was gone (seen by evaluate) at what price
    std::map<std::string, OrderPtr> came_;
    std::map<std::string, double> came_at_;
    std::map<std::string, Decimal> came_price_;
    std::map<std::string, std::optional<Decimal>> held_back_;  // the hold the last price() met there
    std::map<std::string, double> thin_until_;  // side -> clock time its thin-side markup holds until
    std::map<std::string, double> thin_times_;  // side -> how lopsided the book was when last seen thin there
    bool warned_against_ = false;
    bool warned_small_ = false;
    // the view the last targets() of both sides priced and the prices it found
    const MarketView* priced_view_ = nullptr;
    std::map<std::string, std::optional<Decimal>> priced_;
};

}  // namespace nmm
