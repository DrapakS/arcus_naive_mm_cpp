#include "trading/quotes.hpp"

#include <algorithm>
#include <ctime>
#include <format>

#include "util/log.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

std::string px(const std::optional<Decimal>& price) { return price ? fmt(*price) : "-"; }

// Local time of day with milliseconds: "17:18:54.512".
std::string now_ms() {
    std::string t = local_time_us();
    return t.substr(0, t.size() - 3);
}

const std::vector<std::string> SIDES_ {"BUY", "SELL"};

}  // namespace

QuoteWatch::QuoteWatch(QuoteStrategy& strategy_, double every_s_)
    : strategy(strategy_), every_s(every_s_), log(&get_logger(QUOTES_LOGGER)) {}

void QuoteWatch::update(const MarketView& view) {
    auto now_targets = strategy.prices(view);  // as the strategy loop just priced this view, or priced now
    if (now_targets != targets) {
        targets = now_targets;
        if (strategy.gateway == nullptr) {  // counted on every change, shown or not
            for (const auto& m : moves_for(now_targets))
                if (std::find(moved_.begin(), moved_.end(), m) == moved_.end()) moved_.push_back(m);
        }
    }
    if (now_targets == shown_) return;
    double now = clock();
    if (now - last_line_ < every_s) return;
    shown_ = now_targets;
    last_line_ = now;
    std::string tail;
    if (strategy.gateway != nullptr) {
        tail = order_text(view);
    } else {
        tail = moved_.empty() ? "" : "re-place " + join(moved_, ", ");
        moved_.clear();
    }
    std::string text = line(view, tail);
    stdout_write(now_ms() + "  " + text + "\n");
    if (log->has_handlers()) log->info("{}", text);
}

std::vector<std::string> QuoteWatch::moves_for(const std::map<std::string, std::optional<Decimal>>& targets_now) {
    // Where a trading bot would have moved its orders on this update.
    std::vector<std::string> moved;
    for (const auto& [s, p] : targets_now) {
        auto& standing = resting[s];
        if (!p || strategy.paused(s)) {
            if (standing) moved.push_back(lower(s) + " pulled");
            standing.reset();
        } else if (!standing || strategy.needs_move(s, *standing, *p)) {
            standing = p;
            moves[s] += 1;
            moved.push_back(lower(s));
        }
    }
    return moved;
}

std::string QuoteWatch::order_text(const MarketView& view) {
    // The working orders (one, or a buy and a sell): how far from its target, how deep in the book.
    auto orders = strategy.gateway->working();
    if (orders.empty()) return "no order";
    std::vector<std::string> parts;
    for (const auto& o : orders) parts.push_back(one(*o, view));
    return join(parts, "; ");
}

std::string QuoteWatch::one(const Order& o, const MarketView& view) {
    std::vector<std::string> notes;
    const auto& target = targets[o.side];
    if (target) notes.push_back(std::format("{} bp off target", ((o.price - *target).abs() / *target * Decimal(10'000)).fixed(1)));
    if (view.venue) {
        double p = o.price.to_double();
        auto best = o.side == "BUY" ? view.venue->best_bid() : view.venue->best_ask();
        if (best) {
            double behind = o.side == "BUY" ? *best - p : p - *best;
            notes.push_back(std::format("{:.1f} bp deep", std::max(behind, 0.0) / p * 10'000));
        }
        auto others = strategy.best_of_others(view, o.side);
        if (!others || (o.side == "BUY" ? *others < p : *others > p))
            notes.push_back("alone on top");  // nobody else at its price or better
    }
    return "order " + o.str() + (notes.empty() ? "" : " " + join(notes, ", "));
}

std::string QuoteWatch::line(const MarketView& view, const std::string& tail) {
    std::vector<std::string> parts {
        "buy " + px(targets["BUY"]),
        "sell " + px(targets["SELL"]),
        view.fair_bid ? std::format("fair bid {:.2f}", *view.fair_bid) : "fair bid -",
    };
    if (view.model_bps) parts.push_back(std::format("ml {:+.2f} bp", *view.model_bps));
    for (auto [name, other] : {std::pair<const char*, std::optional<double>> {"futures fair bid", view.futures_fair_bid},
                               {"spot fair bid", view.spot_fair_bid}})
        if (other && other != view.fair_bid) parts.push_back(std::format("{} {:.2f}", name, *other));
    if (view.venue) parts.push_back("arcus " + pyrepr(view.venue->best_bid()) + "/" + pyrepr(view.venue->best_ask()));
    if (view.reference) parts.push_back("binance " + pyrepr(view.reference->best_bid()) + "/" + pyrepr(view.reference->best_ask()));
    if (view.futures) parts.push_back("futures " + pyrepr(view.futures->best_bid()) + "/" + pyrepr(view.futures->best_ask()));
    if (view.bybit) parts.push_back("bybit " + pyrepr(view.bybit->best_bid()) + "/" + pyrepr(view.bybit->best_ask()));
    if (view.okx) parts.push_back("okx " + pyrepr(view.okx->best_bid()) + "/" + pyrepr(view.okx->best_ask()));
    if (!tail.empty()) parts.push_back(tail);
    const auto& guards = strategy.guards;
    if (std::any_of(guards.begin(), guards.end(), [](Pauses* g) { return dynamic_cast<LagGuard*>(g) && g->paused("BUY"); }))
        parts.push_back("late data pause");
    for (auto [s, mark] : {std::pair<const char*, const char*> {"BUY", "drop pause"}, {"SELL", "rise pause"}})
        if (std::any_of(guards.begin(), guards.end(), [&](Pauses* g) { return !dynamic_cast<LagGuard*>(g) && g->paused(s); }))
            parts.push_back(mark);
    if (auto thin = strategy.thin_side(view); thin && strategy.widens(thin->first))
        parts.push_back(std::format("{}: {} markup x{:g}", lopsided(*thin, strategy.thin_held), lower(thin->first), strategy.imbalance->factor));
    for (const auto& s : SIDES_)
        if (strategy.drifted(s)) parts.push_back(std::format("{} drift markup x{}", lower(s), pyrepr(strategy.drift_factor_float)));
    if (strategy.at_open()) parts.push_back("market open: no adding");
    return join(parts, "  ");
}

std::string QuoteWatch::status() const {
    // Repeats the current prices: a quiet market prints no quote lines at all.
    std::string now = std::format("quote: now buy {}, sell {}", px(targets.at("BUY")), px(targets.at("SELL")));
    if (strategy.gateway != nullptr) return now;  // the order itself is on the trading: line
    return std::format("{}; orders would stand at buy {}, sell {}; placements so far buy {}, sell {}", now, px(resting.at("BUY")),
                       px(resting.at("SELL")), moves.at("BUY"), moves.at("SELL"));
}

}  // namespace nmm
