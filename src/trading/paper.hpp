// Paper trading: orders live locally and fill against Arcus public trades.
//
// A resting buy fills when a trade prints at or below its price, a sell when a
// trade prints at or above it, up to the trade size. Queue position and the
// effect of our own size on the market are ignored, so fills are optimistic.
//
// A cancel takes CANCEL_LATENCY_S to act, as live, and trades printed before it
// acts still fill the order. That matters: the Arcus book frame that shows a
// sweep (and makes the bot pull its order) often arrives before the trades frame
// of the same sweep, and an instant paper cancel let the order escape exactly the
// fills that cost a live one. A trade printed before the order was placed (by the
// exchange's time) fills nothing.
//
// The trades come over `connections` connections and the first copy of each frame
// counts (paper pair, 19 Oct: one connection, given a server route 74 ms late after
// a reconnect, cost that bot half its fills for the hour after). A connection whose
// copies come slow_link_ms behind the other's at the median is reconnected, and
// every way a connection ends is logged.
#pragma once

#include <map>
#include <string>

#include "feeds/base.hpp"
#include "race.hpp"
#include "trading/gateway.hpp"

namespace nmm {

constexpr double CANCEL_LATENCY_S = 0.025;  // a live cancel acts some 10-25 ms after it is sent

class PaperGateway : public Gateway {
public:
    PaperGateway(MarketSpec market, std::string ws_url, std::function<void()> on_change = {}, int connections = 1,
                 double slow_link_ms = 0.0);

    awaitable<void> run() override;
    std::string status() const override;
    // One frame of the market's trades channel from `link` (tests feed these directly).
    void on_message(const Json& msg, Link* link = nullptr);
    // Reconnect the first trades connection found slow while another is up; returns it.
    Link* check_links(std::optional<double> now = {});

    std::string ws_url;
    std::vector<Link> links;  // the trades connections
    double slow_link_ms;  // reconnect a connection this far behind the first copies; 0 = never
    // The orders whose placement time is kept: those a trade can still fill (tests).
    size_t placements_kept() const { return placed_us_.size(); }

protected:
    void send_place(const OrderPtr& order) override;
    void send_cancel(const OrderPtr& order, const std::string& why = "") override;
    void finish(const OrderPtr& order, OrderState state, std::optional<std::string> reason = {}, bool fresh = true) override;

private:
    awaitable<void> run_link(Link& link);
    awaitable<void> watch_links();
    // Without trades we cannot tell whether the orders would have filled.
    void trades_lost(const std::string& reason);
    void cancel_acts(const OrderPtr& order);
    // The trade happened before `order` was placed: on the exchange's clock, set against ours when the offset is known.
    bool printed_before(const Json& trade, const Order& order) const;

    FirstCopies copies_;
    std::map<std::string, int64_t> placed_us_;  // clientId -> when it was placed (host clock, µs)
};

}  // namespace nmm
