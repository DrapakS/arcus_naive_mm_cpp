#include "trading/roundtrip.hpp"

#include <format>

#include "util/pyfmt.hpp"

namespace nmm {

double RoundTrip::value_bps(const Decimal& cash) const {
    Decimal entry = entry_value();
    return entry ? (cash / entry).to_double() * 1e4 : 0.0;
}

RoundTrips::RoundTrips(Logger& log, Clock clock, Decimal dust, int places)
    : log(&log), clock(std::move(clock)), dust(dust), places(places) {}

void RoundTrips::fill(const std::string& side, Decimal size, Decimal price, bool working) {
    if (size <= 0) return;
    if (untracked && (untracked > 0) != (side == "BUY")) {  // first work off what we did not build
        Decimal used = min(size, abs(untracked));
        untracked += side == "BUY" ? used : -used;
        aside_size_ += used;
        aside_value_ += used * price;
        if (!untracked) aside_closed(side);
        size -= used;
        if (size <= 0) return;
    }
    if (!trip) {
        trip = RoundTrip {};
        trip->side = side;
        trip->opened = clock();
    }
    RoundTrip& t = *trip;
    bool closing = side != t.side;
    Decimal rest = 0;
    if (closing) {
        Decimal held = abs(t.position());
        rest = size - min(size, held);
        size = min(size, held);
    }
    if (side == "BUY") {
        t.bought += size;
        t.bought_value += size * price;
    } else {
        t.sold += size;
        t.sold_value += size * price;
    }
    t.fills += 1;
    if (closing && (t.position() == 0 || (!working && abs(t.position()) < dust))) {
        end(t);
        if (rest > 0) fill(side, rest, price);  // it went through zero: the rest opens the next trip
    }
}

void RoundTrips::settle(const std::string& side) {
    if (!trip) return;
    RoundTrip& t = *trip;
    bool closed = static_cast<bool>(t.side == "BUY" ? t.sold : t.bought);
    if (closed && side != t.side && 0 < abs(t.position()) && abs(t.position()) < dust) end(t);
}

void RoundTrips::end(RoundTrip& t) {
    Decimal left = t.position();
    if (left) {
        Decimal cost = left > 0 ? t.bought_value / t.bought : t.sold_value / t.sold;
        if (left > 0) {
            t.bought_value -= left * cost;
            t.bought -= left;
        } else {
            t.sold_value += left * cost;
            t.sold += left;
        }
        if (!untracked) {
            aside_entry = cost;
        } else if (aside_entry && (untracked > 0) == (left > 0)) {  // adds to it: the mean
            Decimal held = abs(untracked);
            aside_entry = (held * *aside_entry + abs(left) * cost) / (held + abs(left));
        }
        untracked += left;
    }
    if (t.last_mark && t.last_exit) {  // the stretch since the last frame
        double now = clock();
        t.exit_s += now - *t.last_mark;
        if (t.last_at_best) t.at_best_s += now - *t.last_mark;
    }
    close(t, left);
}

void RoundTrips::placed(const std::string& side, Decimal price, std::optional<double> mid) {
    if (!trip || side == trip->side) return;
    RoundTrip& t = *trip;
    t.exits += 1;
    if (!t.first_exit_bps && mid && *mid != 0.0) {
        double away = price.to_double() / *mid - 1.0;
        t.first_exit_bps = (side == "SELL" ? away : -away) * 1e4;
    }
}

void RoundTrips::mark(double bid, double ask, std::optional<std::string> order_side, std::optional<double> order_price) {
    if (!trip || trip->position() == 0) return;
    RoundTrip& t = *trip;
    double now = clock();
    if (t.last_mark && t.last_exit) {
        t.exit_s += now - *t.last_mark;
        if (t.last_at_best) t.at_best_s += now - *t.last_mark;
    }
    bool exiting = order_side.has_value() && *order_side != t.side && order_price.has_value();
    t.last_mark = now;
    t.last_exit = exiting;
    t.last_top = std::make_pair(bid, ask);
    t.last_at_best = exiting && (*order_side == "SELL" ? *order_price <= ask : *order_price >= bid);
    Decimal exit_price = Decimal::from_double(t.position() > 0 ? bid : ask);
    double bps = t.value_bps(t.sold_value - t.bought_value + t.position() * exit_price);
    if (bps < t.worst_bps) {
        t.worst_bps = bps;
        t.worst_after = now - t.opened;
    }
}

void RoundTrips::sync(Decimal position_, std::optional<Decimal> entry) {
    if (position_ == position()) return;
    if (trip) {
        log->warning("=== ROUND TRIP dropped: position {}, our fills make {}", fmt(position_), fmt(position()));
        trip.reset();
    }
    untracked = position_;
    aside_entry = entry;
    aside_size_ = aside_value_ = 0;
    if (position_) {
        log->info("=== position {} set aside: not a round trip of this run{}", fmt(position_),
                  entry && *entry ? std::format(" (the exchange's average entry {})", fmt(*entry)) : "");
    }
}

void RoundTrips::report_final(std::optional<double> bid, std::optional<double> ask) {
    Decimal aside = untracked;
    if (!bid && !ask && trip && trip->last_top) {
        bid = trip->last_top->first;
        ask = trip->last_top->second;
    }
    if ((!trip || trip->position() == 0) && !aside) {
        log->info("=== AT EXIT: flat");
        return;
    }
    std::vector<std::string> parts;
    if (trip && trip->position()) {
        const RoundTrip& t = *trip;
        bool long_ = t.position() > 0;
        std::optional<double> mark_price = long_ ? bid : ask;
        std::string entry = long_ ? avg(t.bought_value, t.bought) : avg(t.sold_value, t.sold);
        std::string text = std::format("{} {} of a round trip open {:.1f} s, {} @ {}", long_ ? "long" : "short",
                                       fmt(abs(t.position())), clock() - t.opened, long_ ? "bought" : "sold", entry);
        if (mark_price) {
            Decimal cash = t.sold_value - t.bought_value + t.position() * Decimal::from_double(*mark_price);
            text += std::format(", worth {:.{}f} at the best {}: {:+.2f} bp (${:+.4f})", *mark_price, places,
                                long_ ? "bid" : "ask", t.value_bps(cash), cash.to_double());
        }
        parts.push_back(text);
    }
    if (aside)
        parts.push_back(fmt(aside) + " set aside"
                        + (aside_entry && *aside_entry ? std::format(" (entry {})", fmt(*aside_entry)) : ""));
    log->info("=== AT EXIT: {}", join(parts, "; "));
}

void RoundTrips::aside_closed(const std::string& side) {
    Decimal size = aside_size_, value = aside_value_;
    std::string verb = side == "SELL" ? "sold" : "bought";
    std::string text = std::format("=== SET-ASIDE position closed: {} {} @ {}", verb, fmt(size), (value / size).fixed(places));
    if (aside_entry && *aside_entry) {
        Decimal entry = *aside_entry;
        Decimal cash = side == "SELL" ? value - size * entry : size * entry - value;
        text += std::format(", entry {}: {:+.2f} bp (${})", fmt(entry), (cash / (size * entry)).to_double() * 1e4,
                            cash.fixed(4, true));
    }
    log->info("{}", text);
    aside_entry.reset();
    aside_size_ = aside_value_ = 0;
}

void RoundTrips::close(RoundTrip t, Decimal left) {
    trip.reset();
    Decimal cash = t.sold_value - t.bought_value;
    count += 1;
    pnl += cash;
    turnover += t.entry_value();
    double total_bps = turnover ? (pnl / turnover).to_double() * 1e4 : 0.0;
    log->info("{}", line(t, cash, total_bps, left));
}

std::string RoundTrips::line(const RoundTrip& t, const Decimal& cash, double total_bps, const Decimal& left) const {
    std::string word = t.side == "BUY" ? "sells" : "buys";
    std::string exit_text;
    if (t.exits) {
        exit_text = std::format("exit: {} {}", t.exits, t.exits != 1 ? word : word.substr(0, word.size() - 1));
        if (t.first_exit_bps) exit_text += std::format(", the first {:+.2f} bp from the mid", *t.first_exit_bps);
        if (t.exit_s > 0) {
            std::string best = t.side == "BUY" ? "ask" : "bid";
            exit_text += std::format(", at the best {} {} of {:.1f} s", best, percent(t.at_best_s / t.exit_s, 0), t.exit_s);
        }
        exit_text += "; ";
    }
    std::string crumb = left ? std::format("; {} left over, under the minimum size", fmt(abs(left))) : "";
    return std::format(
        "=== ROUND TRIP #{} {} {}: bought @ {}, sold @ {}, held {:.1f} s, {:+.2f} bp (${}); worst {:+.2f} bp at {:.1f} s; "
        "{} fills{}; {}all {} trips ${} ({:+.2f} bp)",
        count, t.side == "BUY" ? "long" : "short", fmt(t.bought), avg(t.bought_value, t.bought), avg(t.sold_value, t.sold),
        clock() - t.opened, t.value_bps(cash), cash.fixed(4, true), t.worst_bps, t.worst_after, t.fills, crumb, exit_text,
        count, pnl.fixed(4, true), total_bps);
}

std::string RoundTrips::avg(const Decimal& value, const Decimal& size) const {
    return size ? (value / size).fixed(places) : "-";
}

std::string RoundTrips::status() const {
    double total_bps = turnover ? (pnl / turnover).to_double() * 1e4 : 0.0;
    std::string text = std::format("round trips {}, ${} ({:+.2f} bp)", count, pnl.fixed(4, true), total_bps);
    if (trip)
        text += std::format(", one open since {:.1f} s (worst {:+.2f} bp)", clock() - trip->opened, trip->worst_bps);
    return text;
}

}  // namespace nmm
