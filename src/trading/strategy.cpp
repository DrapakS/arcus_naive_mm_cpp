#include "trading/strategy.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>

#include "util/log.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

Logger& slog() { return get_logger("naive_mm.strategy"); }

const std::vector<std::string> BOTH_SIDES {"BUY", "SELL"};
constexpr double NEG_INF = -std::numeric_limits<double>::infinity();

std::string ticks_text(int64_t n) { return n == 1 ? std::format("{} tick", n) : std::format("{} ticks", n); }

Decimal D(double x) { return Decimal::from_double(x); }

std::optional<double> level_at(const std::vector<Level>& levels, double price) {
    for (const auto& [p, size] : levels)
        if (p == price) return size;
    return std::nullopt;
}

double get_or(const std::map<std::string, double>& m, const std::string& key, double fallback) {
    auto it = m.find(key);
    return it == m.end() ? fallback : it->second;
}

}  // namespace

std::optional<double> others_best(const std::vector<Level>& levels, std::optional<double> price, double size) {
    for (const auto& [level_price, level_size] : levels) {
        if (price && level_price == *price && level_size <= size * (1 + 1e-9)) continue;
        return level_price;
    }
    return std::nullopt;
}

std::string queue_note(const std::vector<Level>& levels, const std::string& side, double price, double tick) {
    if (levels.empty()) return "queue: the side is empty";
    auto better = [&](double p) { return side == "BUY" ? p > price : p < price; };
    double best = levels[0].first;
    auto ticks = static_cast<int64_t>(std::nearbyint((side == "BUY" ? best - price : price - best) / tick));
    std::string where = ticks > 0   ? ticks_text(ticks) + " behind others' best"
                        : ticks == 0 ? "at others' best"
                                     : ticks_text(-ticks) + " ahead of others' best";
    double at = 0.0, ahead = 0.0;
    for (const auto& [p, size] : levels) {
        if (p == price) at += size;
        if (better(p)) ahead += size;
    }
    bool beyond = better(levels.back().first);  // past the last level we can see
    std::string ahead_text = beyond ? std::format("over {:g} ahead (deeper than the top {})", ahead, levels.size())
                                    : std::format("{:g} ahead in the top {}", ahead, levels.size());
    return std::format("queue: {}, {}, {}", where, at ? std::format("joins {:g}", at) : "a new level", ahead_text);
}

std::optional<std::string> gap_note(std::optional<double> gap, std::optional<double> change, const std::string& side) {
    if (!gap) return std::nullopt;
    double sign = side == "BUY" ? 1.0 : -1.0;
    std::string text = std::format("futures {:+.2f} bp vs spot for it", sign * *gap);
    if (change) text += std::format(" ({:+.2f} in 2 s)", sign * *change);
    return text;
}

std::optional<std::string> drift_text(DriftWatch& drift, const std::string& side, double factor) {
    auto move = drift.for_side(side);
    if (!move) return std::nullopt;
    std::string text = std::format("drift {:+.2f} bp in {:g} s for it", *move * 1e4, drift.window_s);
    if (factor != 1 && drift.against(side)) text += ", markup x" + pyrepr(factor);
    return text;
}

std::string lopsided(const std::pair<std::string, double>& thin, bool held) {
    const auto& [side, times] = thin;
    std::string heavy = side == "SELL" ? "bids" : "asks", light = side == "SELL" ? "asks" : "bids";
    bool infinite = std::isinf(times);
    if (held) return infinite ? "held: there were no " + light : std::format("held: {} were {:.1f}x {}", heavy, times, light);
    return infinite ? "no " + light : std::format("{} {:.1f}x {}", heavy, times, light);
}

QuoteStrategy::QuoteStrategy(Gateway* gateway_, MarketSpec market_, StrategyOptions o)
    : direction(o.direction), both(o.direction == "both"), session(o.session), exit_after_s(o.exit_after_s),
      add_guard(o.add_guard), open_guard(o.open_guard), halt(o.halt), halt_close(o.halt_close), vol(o.vol), portfolio(o.portfolio), drift(o.drift),
      drift_factor_float(o.drift_factor), gateway(gateway_), market(std::move(market_)), guards(o.guards), never_lead(o.never_lead),
      imbalance(o.imbalance), came_hold_s(o.came_hold_s), queue_snap(o.queue_snap) {
    if (!(0 <= o.queue_step_share && o.queue_step_share <= 1) || !(o.queue_wall_x > 0))
        throw std::invalid_argument("queue_step_share must be within 0..1 and queue_wall_x > 0");
    queue_step_share = D(o.queue_step_share);
    queue_wall_x = D(o.queue_wall_x);
    if (direction != "long" && direction != "short" && direction != "both")
        throw std::invalid_argument("direction must be \"long\", \"short\" or \"both\"");
    // One side at a time: the sides by role. Both sides: no fixed roles (closing() says by the position).
    open_side = direction == "short" ? "SELL" : "BUY";
    close_side = direction == "short" ? "BUY" : "SELL";
    if (both && !(o.max_position_usd > 0)) throw std::invalid_argument("max_position_usd must be > 0");
    max_position_usd = D(o.max_position_usd);
    if (!(0 < o.max_position_resume && o.max_position_resume <= 1)) throw std::invalid_argument("max_position_resume must be within (0, 1]");
    max_position_resume = D(o.max_position_resume);
    if (!(o.off_hours_markup_factor > 0)) throw std::invalid_argument("off_hours_markup_factor must be > 0");
    // A market with hours (RWA): outside the session the markup of both sides is this many times more.
    off_hours_factor = D(o.off_hours_markup_factor);
    if (o.skew_open < 0 || !(0 <= o.skew_close && o.skew_close <= 1))
        throw std::invalid_argument("skew_open must be >= 0 and skew_close within 0..1");
    skew_open = D(o.skew_open);
    skew_close = D(o.skew_close);
    if (o.exit_after_s < 0 || o.exit_loss < 0) throw std::invalid_argument("exit_after_s and exit_loss must be >= 0 (0 = off)");
    exit_loss = D(o.exit_loss);
    if (!(0 < o.add_limit_share && o.add_limit_share <= 1)) throw std::invalid_argument("add_limit_share must be within (0, 1]");
    add_limit_share = D(o.add_limit_share);
    if (!(0 <= o.open_limit_share && o.open_limit_share < 1)) throw std::invalid_argument("open_limit_share must be within [0, 1)");
    open_limit_share = D(o.open_limit_share);
    if (!(1 <= o.drift_factor && o.drift_factor <= 10)) throw std::invalid_argument("drift_factor must be within 1..10 (1 = off)");
    drift_factor = D(o.drift_factor);
    // An order bigger than the size wanted now goes only when it is bigger by this much: the sizes
    // that follow the price (a share of the limit in dollars) move a few satoshi a tick, and
    // re-placing for that cancelled 36 and 59 orders in a 30 min paper run (17 Oct).
    size_slack_ = max(market.min_order_size, D(o.order_size) * Decimal("0.05"));
    if (!(0 <= o.came_hold_s && o.came_hold_s <= 10)) throw std::invalid_argument("came_hold_s must be within 0..10 (0 = off)");
    markup = D(o.markup);
    // The closing order (the sell of a long, the buy of a short) takes this share less markup.
    close_markup = markup * (Decimal(1) - D(o.close_markup_cut));
    tol = D(o.tol);
    tol_away = o.tol_away ? D(o.tol_away) : tol;
    close_tol_away = o.close_tol_away ? D(o.close_tol_away) : tol;
    order_size = D(o.order_size);
    // Without a gateway it only prices (mode quote), so any size will do.
    if (gateway != nullptr && (order_size < market.min_order_size || market.floor_size(order_size) != order_size))
        throw std::invalid_argument(std::format("order_size {} must be at least {} and a multiple of {}", fmt(order_size),
                                                fmt(market.min_order_size), fmt(market.step_size)));
    held_back_ = {{"BUY", std::nullopt}, {"SELL", std::nullopt}};
}

Decimal QuoteStrategy::position_now() const { return gateway != nullptr ? gateway->position() : Decimal(0); }

bool QuoteStrategy::closing(const std::string& s, std::optional<Decimal> position) const {
    if (!both) return s == close_side;
    Decimal p = position ? *position : position_now();
    return (p > 0 && s == "SELL") || (p < 0 && s == "BUY");
}

bool QuoteStrategy::against(const Decimal& position) const {
    if (both) return false;
    return direction == "long" ? position < 0 : position > 0;
}

bool QuoteStrategy::widens(const std::string& s) const {
    return imbalance && (imbalance->sides == "both" || !closing(s));
}

std::string QuoteStrategy::side(const Decimal& position, const MarketView* view) {
    if (against(position)) {
        if (!warned_against_) {
            slog().warning("position {} is against direction {}: taking it back to zero first", fmt(position), direction);
            warned_against_ = true;
        }
        return open_side;
    }
    Decimal size = position.abs();
    OrderPtr o = gateway != nullptr ? gateway->order() : nullptr;
    if (size > 0 && o && o->side == close_side) return close_side;
    if (size >= market.min_order_size && big_enough(size, close_side, view)) return close_side;
    return open_side;
}

bool QuoteStrategy::big_enough(const Decimal& size, const std::string& s, const MarketView* view) {
    // `size` at the price an order on `s` would go now meets the market's minimum notional (true
    // when there is no price to tell by: no order goes out without one anyway).
    const Decimal& notional = market.min_order_notional;
    if (!notional || view == nullptr) return true;
    auto p = price(s, *view);
    return !p || size * *p >= notional;
}

std::optional<Quote> QuoteStrategy::target(const MarketView& view, const Decimal& position) {
    std::string s = side(position, &view);
    auto p = price(s, view);
    if (!p) return std::nullopt;
    Decimal sz = size(s, position, *p);
    if (s == open_side && at_open()) {
        if (!against(position)) return std::nullopt;  // around a market open: nothing that opens a position
        sz = market.floor_size(position.abs());  // it closes one against the direction, not past zero
        if (sz < market.min_order_size || sz * *p < market.min_order_notional) return std::nullopt;
    }
    if (s == open_side && sz * *p < market.min_order_notional) {
        if (!warned_small_) {  // the exchange would reject every one of them
            slog().error("order_size {} at {} is under the minimum notional {}: not quoting, raise order_size", fmt(sz), fmt(*p),
                         fmt(market.min_order_notional));
            warned_small_ = true;
        }
        return std::nullopt;
    }
    return Quote {s, *p, sz};
}

Decimal QuoteStrategy::size(const std::string& s, const Decimal& position, const Decimal& p) const {
    // The whole position when it closes it, else order_size. A position against the direction is
    // closed by the opening side; when it is too small to be an order alone, order_size is added,
    // so the one order takes it back to zero and opens our own.
    if (s == close_side) return market.floor_size(position.abs());
    if (!against(position)) return order_size;
    Decimal sz = market.floor_size(position.abs());
    if (sz >= market.min_order_size && sz * p >= market.min_order_notional) return sz;
    return market.floor_size(position.abs() + order_size);
}

std::optional<Decimal> QuoteStrategy::price(const std::string& s, const MarketView& view) {
    if (!view.fair_bid || !view.venue) return std::nullopt;
    auto bid = view.venue->best_bid(), ask = view.venue->best_ask();
    if (!bid || !ask) return std::nullopt;
    Decimal fair = D(*view.fair_bid);
    bool is_closing = closing(s);
    bool is_urgent = is_closing && urgent(view);
    Decimal m = is_closing ? close_markup : markup;
    if (both && (skew_open || skew_close)) {
        Decimal u = used(fair);
        if (u) m *= is_closing ? max(Decimal(0), Decimal(1) - skew_close * u) : Decimal(1) + skew_open * u;
    }
    Decimal quoted = fair;  // the fair the price is taken from: moved against the class's lean (portfolio shift_bp)
    if (portfolio != nullptr) {
        m *= portfolio->multiplier(s, position_now(), fair);
        if (Decimal shift = portfolio->shift(position_now(), fair)) quoted = fair * (Decimal(1) - shift);
    }
    if (vol != nullptr) m *= vol->factor();
    if (is_urgent) m = Decimal(0);
    if (session != nullptr && off_hours_factor != 1 && session->outside()) m *= off_hours_factor;
    if (drifted(s)) m *= drift_factor;
    if (auto thin = thin_side(view); thin && thin->first == s && widens(s)) m *= D(imbalance->factor);
    Decimal result;
    bool queued = !is_urgent && (queue_snap || queue_step_share > 0);
    bound_[s] = std::nullopt;
    queue_note_[s].clear();
    if (s == "BUY") {
        Decimal best_ask = D(*ask);
        Decimal cap = best_ask - market.tick_at(best_ask);
        if (never_lead)
            if (auto others = best_of_others(view, "BUY")) cap = min(cap, D(*others));  // join the best bid at most, never improve it
        result = market.floor_price(min(quoted * (Decimal(1) - m), cap));
        if (queued) result = queue_price(s, view, result, quoted * m * queue_step_share, cap);
        if (is_urgent) {  // join the best bid (the others' with never_lead), still under the ask
            std::optional<double> join = never_lead ? best_of_others(view, "BUY") : std::nullopt;
            Decimal j = D(join ? *join : *bid);
            result = min(best_ask - market.tick_at(best_ask), max(result, j));
        }
    } else {
        Decimal best_bid = D(*bid);
        Decimal tick = market.tick_at(best_bid);
        Decimal floor = best_bid + tick;
        if (never_lead)
            if (auto others = best_of_others(view, "SELL")) floor = max(floor, D(*others));
        result = market.ceil_price(max(floor, (quoted + tick) * (Decimal(1) + m)));
        if (queued) result = queue_price(s, view, result, quoted * m * queue_step_share, floor);
        if (is_urgent) {  // join the best ask (the others' with never_lead), still over the bid
            std::optional<double> join = never_lead ? best_of_others(view, "SELL") : std::nullopt;
            Decimal j = D(join ? *join : *ask);
            result = max(best_bid + tick, min(result, j));
        }
    }
    held_back_[s] = std::nullopt;
    if (auto held = came_hold(s); held && (s == "BUY" ? result >= *held : result <= *held)) {
        // the book just came to our order at `held`: not back there yet, one tick behind it
        result = s == "BUY" ? *held - market.tick_at(*held) : *held + market.tick_at(*held);
        held_back_[s] = held;
    }
    if (result > 0) return result;
    return std::nullopt;
}

std::optional<Decimal> QuoteStrategy::came_hold(const std::string& s) const {
    if (!came_hold_s) return std::nullopt;
    auto it = came_price_.find(s);
    if (it == came_price_.end()) return std::nullopt;
    if (clock() - get_or(came_at_, s, NEG_INF) < came_hold_s) return it->second;
    return std::nullopt;
}

bool QuoteStrategy::drifted(const std::string& s) const { return drift != nullptr && drift_factor != 1 && drift->against(s); }

std::optional<std::string> QuoteStrategy::drift_note(const std::string& s) const {
    if (drift == nullptr) return std::nullopt;
    return drift_text(*drift, s, drift_factor_float);
}

Decimal QuoteStrategy::used(const Decimal& p) const { return min(Decimal(1), position_now().abs() * p / max_position_usd); }

bool QuoteStrategy::urgent(const MarketView& view) {
    Gateway* gw = gateway;
    Decimal position = position_now();
    std::optional<std::string> why;
    if (gw != nullptr && position != 0 && (exit_after_s || exit_loss)) {
        auto since = gw->position_since;
        auto entry = gw->entry_price;
        if (exit_after_s && since) {
            double held = gw->clock() - *since;
            if (held >= exit_after_s) why = std::format("held {:.0f} s", held);
        }
        auto bid = view.venue->best_bid(), ask = view.venue->best_ask();
        if (!why && exit_loss && entry && bid && ask) {
            Decimal mid = (D(*bid) + D(*ask)) / Decimal(2);
            Decimal loss = position > 0 ? (*entry - mid) / *entry : (mid - *entry) / *entry;
            if (loss >= exit_loss) why = std::format("{} bp under water (entry {})", (loss * Decimal(10'000)).fixed(1), fmt(*entry));
        }
    }
    auto since = gw != nullptr ? gw->position_since : std::nullopt;
    if (why && urgent_ != since) {
        urgent_ = since;
        slog().info("position {} {}: closing at the best price, no markup", fmt(position), *why);
    } else if (!why && urgent_ && urgent_ == since) {
        urgent_.reset();  // (back above the loss line: the markup again)
        slog().info("position {}: closing with its markup again", fmt(position));
    }
    return why.has_value();
}

std::optional<std::pair<std::string, double>> QuoteStrategy::thin_side(const MarketView& view) {
    if (!imbalance || !view.venue) return std::nullopt;
    double now = clock();
    double bids = volume(view.venue->bids, "BUY");
    double asks = volume(view.venue->asks, "SELL");
    std::optional<std::pair<std::string, double>> thin;
    double inf = std::numeric_limits<double>::infinity();
    if (bids > asks * imbalance->ratio) thin = std::pair {std::string("SELL"), asks ? bids / asks : inf};
    else if (asks > bids * imbalance->ratio) thin = std::pair {std::string("BUY"), bids ? asks / bids : inf};
    thin_held = false;
    if (thin) {
        thin_until_[thin->first] = now + imbalance->hold_s;
        thin_times_[thin->first] = thin->second;
        return thin;
    }
    std::optional<std::string> side_held;
    for (const auto& [s, until] : thin_until_)  // the latest one wins if both are held
        if (until > now && (!side_held || until > thin_until_[*side_held])) side_held = s;
    if (!side_held) return std::nullopt;
    thin_held = true;
    return std::pair {*side_held, thin_times_[*side_held]};
}

std::optional<std::pair<double, double>> QuoteStrategy::own_in_book(const std::string& s, const std::vector<Level>* levels) {
    // (price, size) of our order on `s` that the public book can be showing: the working order once
    // Order::in_book, else one cancelled moments ago (the book can show its level a little after the
    // orders channel says CANCELED, and a new order placed then would take it for someone else's and
    // join itself) while `levels` still show its remaining at its price; once they show less there,
    // or nothing within their range, it is gone and forgotten.
    Gateway* gw = gateway;
    if (gw == nullptr || !gw->orders_in_public_book) return std::nullopt;
    if (auto it = gw->orders.find(s); it != gw->orders.end()) {
        Order& o = *it->second;
        if (!o.book_seen && o.book_base && levels != nullptr) {
            // The book shows it once its level has grown by its size since it was placed.
            double p = o.price.to_double();
            double at = level_at(*levels, p).value_or(0.0);
            if (at >= *o.book_base + o.remaining().to_double() * (1 - 1e-9)) o.book_seen = true;
        }
        if (o.in_book()) return std::pair {o.price.to_double(), o.remaining().to_double()};
    }
    auto gone = gw->recently_cancelled(s);
    if (!gone || levels == nullptr) return gone;
    auto [p, size_] = *gone;
    auto at = level_at(*levels, p);
    if (!at) {
        bool beyond = !levels->empty() && (s == "BUY" ? levels->back().first > p : levels->back().first < p);
        if (beyond) return gone;  // deeper than the levels shown: cannot tell
    } else if (*at >= size_ * (1 - 1e-9)) {
        return gone;  // still showing
    }
    gw->forget_cancelled(s);
    return std::nullopt;
}

Decimal QuoteStrategy::queue_price(const std::string& s, const MarketView& view, const Decimal& raw, const Decimal& budget,
                                   const Decimal& limit) {
    // [queue]: `raw` is the markup's price; `budget` how much nearer the market a step over a wall may
    // go; `limit` the nearest price allowed at all (a tick off the other side, the others' best with
    // never_lead). Sets bound_ (the nearest price an order may stand at) and the note.
    bool buy = s == "BUY";
    std::vector<Level> levels = others_levels(buy ? view.venue->bids : view.venue->asks, s);
    auto nearer = [&](const Decimal& a, const Decimal& b) { return buy ? a > b : a < b; };  // a nearer the market than b
    auto past = [&](double p) {  // one tick past the level at p, toward the market
        Decimal level = D(p);
        return buy ? level + market.tick_at(level) : level - market.tick_at(level);
    };
    double t = raw.to_double();
    bound_[s] = raw;
    if (queue_step_share > 0) {
        double wall = (order_size * queue_wall_x).to_double();
        std::optional<Level> over;
        for (const auto& level : levels) {
            if (level.second < wall || (buy ? level.first < t : level.first > t)) continue;  // small, or behind us already
            Decimal step = past(level.first);
            if ((step - raw).abs() > budget || nearer(step, limit)) continue;
            if (!over || (buy ? level.first > over->first : level.first < over->first)) over = level;
        }
        if (over) {
            Decimal step = past(over->first);
            bound_[s] = step;
            queue_note_[s] = std::format("queue: stepped over {:g} at {:g}, {} bp nearer than the markup's {}", over->second, over->first,
                                         ((step - raw).abs() / raw * Decimal(10'000)).fixed(2), fmt(raw));
            return step;
        }
    }
    if (!queue_snap) return raw;
    // the others' level at our price or the nearest one behind it; none seen there: the gap is unknown
    std::optional<double> behind;
    for (const auto& [p, size_] : levels)
        if ((buy ? p <= t : p >= t) && (!behind || (buy ? p > *behind : p < *behind))) behind = p;
    if (!behind) return raw;
    Decimal snapped = past(*behind);
    if (!nearer(raw, snapped)) return raw;  // at a level, or a tick past it already
    queue_note_[s] = std::format("queue: {} bp back from the markup's {}, a tick past the others' {:g}",
                                 ((raw - snapped).abs() / raw * Decimal(10'000)).fixed(2), fmt(raw), *behind);
    return snapped;
}

std::vector<Level> QuoteStrategy::others_levels(const std::vector<Level>& levels, const std::string& s) {
    auto own = own_in_book(s, &levels);
    if (!own) return levels;
    auto [p, size_] = *own;
    std::vector<Level> out;
    for (auto [level_price, level_size] : levels) {
        if (level_price == p) {
            if (level_size <= size_ * (1 + 1e-9)) continue;  // only us
            level_size -= size_;
        }
        out.emplace_back(level_price, level_size);
    }
    return out;
}

double QuoteStrategy::volume(const std::vector<Level>& levels, const std::string& s) {
    // Volume on the first levels of a side, our own order taken out.
    auto own = own_in_book(s, &levels);
    double total = 0.0;
    size_t n = std::min(levels.size(), static_cast<size_t>(std::max(imbalance->levels, 0)));
    for (size_t i = 0; i < n; ++i) {
        auto [p, size_] = levels[i];
        total += own && p == own->first ? std::max(size_ - own->second, 0.0) : size_;
    }
    return total;
}

std::optional<double> QuoteStrategy::best_of_others(const MarketView& view, const std::string& s) {
    const std::vector<Level>& levels = s == "BUY" ? view.venue->bids : view.venue->asks;
    auto own = own_in_book(s, &levels);
    return own ? others_best(levels, own->first, own->second) : others_best(levels);
}

void QuoteStrategy::evaluate(const MarketView& view) {
    priced_view_ = nullptr;  // priced afresh for this view
    Gateway* gw = gateway;
    for (auto it = came_.begin(); it != came_.end();) {  // first thing: whatever else holds placing back
        if (terminal(it->second->state)) {
            if (it->second->state == OrderState::CANCELED) {  // the pause runs from its CANCELED (this is the next look)
                came_at_[it->first] = clock();
                came_price_[it->first] = it->second->price;
            }
            it = came_.erase(it);
        } else {
            ++it;
        }
    }
    if (!gw->ready) {
        check_cancel(view);  // placing waits, pulling an order that has to go does not
        return;
    }
    if (both) {
        evaluate_both(view);
        return;
    }
    OrderPtr o = gw->order();
    // A placement not yet acknowledged can still be cancelled (by clientId), so a move right after
    // placing is acted on at once; anything else in flight waits for its answer. A pause after a
    // rejected placement holds back placing only.
    bool cancellable = o && (o->state == OrderState::OPEN || o->state == OrderState::PENDING_NEW) && !gw->cancel_cooling_down();
    if (gw->busy() && !cancellable) return;
    auto quote = target(view, gw->position());
    if (o) {
        if (auto reason = cancel_reason(o, quote, view)) gw->cancel(o, *reason);
        return;
    }
    if (!quote || paused(quote->side)) return;
    if (clock() - get_or(came_at_, quote->side, NEG_INF) < REPLACE_AFTER_CAME_S)
        return;  // the book just came to our order there: the market is on its way, let it pass
    place(*quote, view);
}

void QuoteStrategy::evaluate_both(const MarketView& view) {
    // Both sides at once: each side's order on its own, as one side's alone. A side waiting for an
    // answer (a cancel in flight) holds back only itself.
    // A pause after a rejected placement holds back placing, not cancelling.
    Gateway* gw = gateway;
    if (gw->cancel_cooling_down()) return;
    auto quotes = targets(view, gw->position());
    for (const auto& s : BOTH_SIDES) {
        auto it = gw->orders.find(s);
        OrderPtr o = it == gw->orders.end() ? nullptr : it->second;
        const auto& quote = quotes[s];
        if (o) {
            if (o->state == OrderState::OPEN || o->state == OrderState::PENDING_NEW)
                if (auto reason = cancel_reason(o, quote, view)) gw->cancel(o, *reason);
            continue;
        }
        if (!quote || paused(s) || gw->cooling_down()) continue;
        if (clock() - get_or(came_at_, s, NEG_INF) < REPLACE_AFTER_CAME_S) continue;
        place(*quote, view);
    }
}

std::map<std::string, std::optional<Quote>> QuoteStrategy::targets(const MarketView& view, const Decimal& position,
                                                                   const std::vector<std::string>& sides) {
    std::map<std::string, std::optional<Quote>> out;
    std::map<std::string, std::optional<Decimal>> found;
    for (const auto& s : sides) {
        auto p = found[s] = price(s, view);
        std::optional<Decimal> sz = p ? both_size(s, position, *p) : std::nullopt;
        out[s] = sz ? std::optional(Quote {s, *p, *sz}) : std::nullopt;
    }
    if (found.size() == BOTH_SIDES.size()) {
        priced_view_ = &view;
        priced_ = found;
    }
    return out;
}

std::map<std::string, std::optional<Decimal>> QuoteStrategy::prices(const MarketView& view) {
    if (priced_view_ == &view) return priced_;
    std::map<std::string, std::optional<Decimal>> out;
    for (const auto& s : BOTH_SIDES) out[s] = price(s, view);
    return out;
}

std::optional<Decimal> QuoteStrategy::both_size(const std::string& s, const Decimal& position, const Decimal& p) {
    const MarketSpec& m = market;
    if (order_size * p < m.min_order_notional) {
        if (!warned_small_) {  // the exchange would reject every one of them
            slog().error("order_size {} at {} is under the minimum notional {}: not quoting, raise order_size", fmt(order_size),
                         fmt(p), fmt(m.min_order_notional));
            warned_small_ = true;
        }
        return std::nullopt;
    }
    Decimal cap = max_position_usd / p;
    Decimal room = s == "BUY" ? cap - position : cap + position;
    Decimal sz = room > 0 ? m.floor_size(min(order_size, room)) : Decimal(0);
    // Held once no valid order fits under the limit; back once the position on this side is under
    // max_position_resume of it (95%: under $950 of $1000), not each time the price ticks.
    bool is_limited = sz < m.min_order_size || sz * p < m.min_order_notional;
    if (limited_[s] && !is_limited) {
        Decimal held = (s == "BUY" ? position : -position) * p;  // the position on this side, dollars
        is_limited = held >= max_position_usd * max_position_resume;
    }
    if (is_limited != limited_[s]) {
        limited_[s] = is_limited;
        std::string what = is_limited
                               ? std::format("no more {}, at the limit of ${}", s == "BUY" ? "buying" : "selling", fmt(max_position_usd))
                               : std::format("{} again, under the limit", s == "BUY" ? "buying" : "selling");
        slog().info("position {} (${:.2f} at {}): {}", fmt(position), (position * p).to_double(), fmt(p), what);
    }
    if (is_limited) return std::nullopt;
    if (closing(s, position)) {
        if (at_open()) {  // around a market open: the closing order does not go through zero
            Decimal closing_sz = m.floor_size(position.abs());
            if (closing_sz >= m.min_order_size && closing_sz * p >= m.min_order_notional) return closing_sz;
            return std::nullopt;
        }
        return closing_size(position, room);
    }
    auto capped = add_cap(s, sz, position, p);
    if (!capped) return std::nullopt;
    return open_cap(s, *capped, position, p);
}

bool QuoteStrategy::halted() const { return halt != nullptr && !halt_close && halt->active(); }

bool QuoteStrategy::closing_only() const { return halt != nullptr && halt_close && halt->active(); }

bool QuoteStrategy::at_open() const { return (open_guard != nullptr && open_guard->active()) || closing_only(); }

std::optional<Decimal> QuoteStrategy::open_cap(const std::string& s, const Decimal& sz, const Decimal& position, const Decimal& p) const {
    // Both sides: within a market-open window an opening order is not placed, or with
    // open_limit_share takes the position on its side to that share of the limit at most.
    if (!at_open()) return sz;
    if (!open_limit_share || closing_only()) return std::nullopt;
    Decimal cap = max_position_usd * open_limit_share / p;
    Decimal room = s == "BUY" ? cap - position : cap + position;
    Decimal capped = room > 0 ? market.floor_size(min(sz, room)) : Decimal(0);
    if (capped >= market.min_order_size && capped * p >= market.min_order_notional) return capped;
    return std::nullopt;
}

Decimal QuoteStrategy::closing_size(const Decimal& position, const Decimal& room) const {
    // Both sides: the closing order is the bigger of the whole position and order_size (within the
    // limit on the other side): a position bigger than one order goes in one order, no crumbs left;
    // a smaller one is closed by a whole order, which goes through zero by the difference.
    return market.floor_size(min(max(position.abs(), order_size), room));
}

std::optional<Decimal> QuoteStrategy::add_cap(const std::string& s, const Decimal& sz, const Decimal& position, const Decimal& p) {
    // Both sides: while add_guard holds `s` after a move against it, an opening order takes the
    // position on that side to add_limit_share of the limit at most (from flat, an order under that
    // share still goes).
    bool held = add_held_[s];
    if (add_guard == nullptr || !add_guard->paused(s)) {
        if (held) {
            add_held_[s] = false;
            slog().info("{}: adding allowed again up to the limit", s == "BUY" ? "buying" : "selling");
        }
        return sz;
    }
    Decimal cap = max_position_usd * add_limit_share / p;
    Decimal room = s == "BUY" ? cap - position : cap + position;
    Decimal capped = room > 0 ? market.floor_size(min(sz, room)) : Decimal(0);
    bool valid = capped >= market.min_order_size && capped * p >= market.min_order_notional;
    if (!held && capped < sz) {
        add_held_[s] = true;
        slog().info("position {}: after a {}, {} at most to {:.0f}% of the limit", fmt(position), s == "BUY" ? "fall" : "rally",
                    s == "BUY" ? "buying" : "selling", (add_limit_share * Decimal(100)).to_double());
    }
    if (valid) return capped;
    return std::nullopt;
}

void QuoteStrategy::place(const Quote& quote, const MarketView& view) {
    Gateway* gw = gateway;
    // Where the others stood on our side as we placed: every level is someone else's but one our
    // order just cancelled may still show at.
    const std::vector<Level>& raw = quote.side == "BUY" ? view.venue->bids : view.venue->asks;
    std::vector<Level> levels = others_levels(raw, quote.side);
    std::optional<double> others = levels.empty() ? std::nullopt : std::optional(levels[0].first);
    std::string why = std::format("fair bid {:.2f}, Arcus {}/{}", *view.fair_bid, pyrepr(view.venue->best_bid()),
                                  pyrepr(view.venue->best_ask()));
    for (auto [label, other] : {std::pair<const char*, std::optional<double>> {"futures fair bid", view.futures_fair_bid},
                                {"spot fair bid", view.spot_fair_bid}})
        if (other && other != view.fair_bid) why += std::format(", {} {:.2f}", label, *other);  // the other reference's fair, for the logs
    auto thin = thin_side(view);
    bool widened = thin && thin->first == quote.side && widens(quote.side);
    if (widened) why += "; " + lopsided(*thin, thin_held);
    why += "; " + queue_note(levels, quote.side, quote.price.to_double(), market.tick_at(quote.price).to_double());
    if (const std::string& note = queue_note_[quote.side]; !note.empty()) why += "; " + note;
    if (auto note = gap_note(view.fair_gap_bps, view.fair_gap_change_bps, quote.side)) why += "; " + *note;
    if (auto note = drift_note(quote.side)) why += "; " + *note;
    if (vol != nullptr)
        if (auto note = vol->note()) why += "; " + *note;
    if (portfolio != nullptr && view.fair_bid)
        if (auto note = portfolio->note(quote.side, position_now(), D(*view.fair_bid))) why += "; " + *note;
    if (auto held = held_back_[quote.side]; held && came_at_.count(quote.side))
        why += std::format("; the book came to our order at {} {:.0f} ms ago: one tick behind it for {:.0f} ms", fmt(*held),
                           (clock() - came_at_[quote.side]) * 1000, came_hold_s * 1000);
    auto bid = view.venue->best_bid(), ask = view.venue->best_ask();
    std::optional<double> mid = bid && ask ? std::optional((*bid + *ask) / 2) : std::nullopt;
    OrderPtr o = gw->place(quote.side, quote.price, quote.size, why, mid);
    // What the book showed at its price as it went out: once that grows by its size, it shows us.
    double p = quote.price.to_double();
    auto shown = level_at(raw, p);
    bool reach = !raw.empty() && (quote.side == "BUY" ? raw.back().first <= p : raw.back().first >= p);
    o->book_base = shown ? shown : (reach ? std::optional(0.0) : std::nullopt);
    placed_[quote.side] = {o, others, widened};
}

void QuoteStrategy::check_cancel(const MarketView& view) {
    Gateway* gw = gateway;
    if (!gw->can_cancel() || gw->cancel_cooling_down()) return;
    if (both) {
        std::vector<OrderPtr> working;
        std::vector<std::string> sides;
        for (const auto& o : gw->working())
            if (o->state == OrderState::OPEN || o->state == OrderState::PENDING_NEW) {
                working.push_back(o);
                sides.push_back(o->side);
            }
        if (!working.empty()) {  // priced for the sides with an order only: the loop prices both when it places
            auto quotes = targets(view, gw->position(), sides);
            priced_view_ = nullptr;  // a view of the moment, not the strategy loop's
            for (const auto& o : working)
                if (auto reason = cancel_reason(o, quotes[o->side], view)) gw->cancel(o, *reason);
        }
        return;
    }
    OrderPtr o = gw->order();
    if (!o || (o->state != OrderState::OPEN && o->state != OrderState::PENDING_NEW)) return;
    if (auto reason = cancel_reason(o, target(view, gw->position()), view)) gw->cancel(o, *reason);
}

bool QuoteStrategy::paused(const std::string& s) const {
    return std::any_of(guards.begin(), guards.end(), [&](const Pauses* g) { return g->paused(s); }) || halted();
}

void QuoteStrategy::pull(const std::string& s, const std::string& why) {
    Gateway* gw = gateway;
    auto it = gw->orders.find(s);
    if (it == gw->orders.end()) return;
    OrderPtr o = it->second;
    if (o->state != OrderState::OPEN && o->state != OrderState::PENDING_NEW) return;
    if (gw->can_cancel() && !gw->cancel_cooling_down()) gw->cancel(o, why);
}

std::optional<std::string> QuoteStrategy::cancel_reason(const OrderPtr& o, const std::optional<Quote>& quote, const MarketView& view) {
    if (halted()) return "market open: no trading";
    if (!quote) {
        if (both && limited_[o->side])
            return std::format("position {} is at the limit of ${}", fmt(gateway->position()), fmt(max_position_usd));
        if (both && add_held_[o->side])
            return std::format("no adding past {}% of the limit after a move against it", (add_limit_share * Decimal(100)).fixed(0));
        if (both && closing_only() && !closing(o->side)) return "market open: closing only";
        return "no price to quote at";
    }
    if (both && paused(o->side)) return "a guard holds this side";  // a trend move past both extremes pauses both sides at once
    if (both && o->remaining() - quote->size > size_slack_ && (closing(o->side) || add_held_[o->side])) {
        // A closing order bigger than both the position and order_size (the position shrank under
        // it), or an opening one bigger than the add guard allows.
        return std::format("{} left of it is more than the {} wanted now", fmt(o->remaining()), fmt(quote->size));
    }
    Decimal position = gateway->position().abs();
    if (both && closing(o->side)) {
        Decimal held = market.floor_size(position);
        if (o->remaining() < held)  // the position grew past it (a fill on the other side): the closing order takes all of it
            return std::format("{} left of it is less than the position {}", fmt(o->remaining()), fmt(held));
    }
    if (o->side != quote->side) return std::format("position {}, switching to {}", fmt(gateway->position()), quote->side);
    if (!both && o->side == close_side && o->remaining() > position) {
        // The position shrank under it (a late fill of an earlier order, a stray, a reconnect):
        // filled whole, it would leave a position against the direction.
        return std::format("{} left of it is more than the position {}", fmt(o->remaining()), fmt(position));
    }
    if (never_lead) {
        if (auto reason = book_came(o, view)) {
            came_[o->side] = o;
            return reason;
        }
    }
    // An order placed under the thin-side markup is deeper than the target once the book has evened
    // out: brought back by tol, or tol_away would leave it stranded there.
    auto placed = placed_.find(o->side);
    bool stranded = false;
    if (placed != placed_.end() && std::get<0>(placed->second) == o && std::get<2>(placed->second)) {
        auto thin = thin_side(view);
        stranded = !(thin && thin->first == o->side);
    }
    if (needs_move(o->side, o->price, quote->price, stranded, bound_[o->side])) {
        Decimal off = (o->price - quote->price).abs() / quote->price;
        return std::format("target {} is {} bp away{}", fmt(quote->price), (off * Decimal(10'000)).fixed(2),
                           stranded ? ", the thin-side markup is over" : "");
    }
    return std::nullopt;
}

bool QuoteStrategy::needs_move(const std::string& s, const Decimal& p, const Decimal& target_price, bool strict,
                               const std::optional<Decimal>& bound) const {
    if (bound && !strict && *bound != target_price) {  // [queue]: between the target and the bound it stays
        bool beyond = s == "BUY" ? p > *bound : p < *bound;
        if (beyond) return (p - *bound).abs() / *bound > tol;
        if (s == "BUY" ? p >= target_price : p <= target_price) return false;
    }
    Decimal off = (p - target_price).abs() / target_price;
    bool toward = s == "BUY" ? target_price < p : target_price > p;
    if (toward || strict) return off > tol;
    return off > (closing(s) ? close_tol_away : tol_away);
}

std::optional<std::string> QuoteStrategy::book_came(const OrderPtr& o, const MarketView& view) {
    // The book came to the order: the others' best price on its side is no longer better than the
    // order and got there by moving toward it (the levels ahead vanished), or everyone else left
    // the order's level.
    if (!view.venue) return std::nullopt;
    auto others = best_of_others(view, o->side);
    double p = o->price.to_double();
    auto placed = placed_.find(o->side);
    double ref = p;
    if (placed != placed_.end() && std::get<0>(placed->second) == o && std::get<1>(placed->second)) ref = *std::get<1>(placed->second);
    std::string word = o->side == "BUY" ? "bid" : "ask";
    if (!others) return "the book came to it: no other " + word + " left";
    bool came = o->side == "BUY" ? (*others < ref && *others <= p) : (*others > ref && *others >= p);
    if (!came) return std::nullopt;
    return std::format("the book came to it: others' best {} {:g}, {:g} when placed", word, *others, ref);
}

}  // namespace nmm
