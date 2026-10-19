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
#pragma once

#include <map>
#include <string>

#include "trading/gateway.hpp"

namespace nmm {

constexpr double CANCEL_LATENCY_S = 0.025;  // a live cancel acts some 10-25 ms after it is sent

class PaperGateway : public Gateway {
public:
    PaperGateway(MarketSpec market, std::string ws_url, std::function<void()> on_change = {});

    awaitable<void> run() override;
    // One frame of the market's trades channel (tests feed these directly).
    void on_message(const Json& msg);

    std::string ws_url;

protected:
    void send_place(const OrderPtr& order) override;
    void send_cancel(const OrderPtr& order, const std::string& why = "") override;

private:
    void cancel_acts(const OrderPtr& order);
    // The trade happened before `order` was placed: on the exchange's clock, set against ours when the offset is known.
    bool printed_before(const Json& trade, const Order& order) const;

    std::map<std::string, int64_t> placed_us_;  // clientId -> when it was placed (host clock, µs)
};

}  // namespace nmm
