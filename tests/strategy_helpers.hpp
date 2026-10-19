// Helpers of the strategy and quote line tests: a gateway that records requests (the
// test plays the exchange), market fixtures and views of the Arcus book.
#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "orderbook.hpp"
#include "trading/gateway.hpp"
#include "trading/market.hpp"
#include "trading/strategy.hpp"
#include "util/decimal.hpp"
#include "util/json.hpp"
#include "view.hpp"

namespace nmm {

// The strategy's own state, for the tests that set it as the Python tests do.
struct QuoteStrategyTestAccess {
    static std::map<std::string, Decimal>& came_price(QuoteStrategy& s) { return s.came_price_; }
    static std::map<std::string, double>& came_at(QuoteStrategy& s) { return s.came_at_; }
};

inline std::ostream& operator<<(std::ostream& out, const Decimal& d) { return out << d.str(); }

}  // namespace nmm

namespace nmm::test::strategy {

inline Decimal D(const char* text) { return Decimal(text); }

inline MarketSpec btc_spec() {
    return MarketSpec::from_json(parse_json(R"({
        "marketId": 1, "marketDisplayName": "BTC-USD", "tickSize": "0.1", "stepSize": "0.00000001",
        "tickTiers": [{"upToPrice": "500000", "tick": "0.1"}, {"upToPrice": "1000000", "tick": "0.2"}, {"tick": "5"}],
        "minOrderSize": "0.0001", "minOrderNotional": "5", "maxOrderSize": "10000"})"));
}

inline MarketSpec eth_spec() {
    return MarketSpec::from_json(parse_json(R"({
        "marketId": 2, "marketDisplayName": "ETH-USD", "tickSize": "0.01", "stepSize": "0.0000001",
        "tickTiers": [{"upToPrice": "10000", "tick": "0.01"}, {"tick": "0.1"}],
        "minOrderSize": "0.001", "minOrderNotional": "5", "maxOrderSize": "100000"})"));
}

// One request the strategy made: (kind, side, price, size).
struct Action {
    std::string kind, side;
    Decimal price, size;
    bool operator==(const Action&) const = default;
};

inline std::ostream& operator<<(std::ostream& out, const Action& a) {
    return out << "(" << a.kind << ", " << a.side << ", " << a.price.str() << ", " << a.size.str() << ")";
}

// Records requests; the test plays the exchange.
class FakeGateway : public Gateway {
public:
    explicit FakeGateway(MarketSpec m = btc_spec()) : Gateway(std::move(m)) { ready = true; }

    std::vector<std::pair<std::string, OrderPtr>> sent;
    std::string why;

    using Gateway::fill;
    using Gateway::finish;
    using Gateway::set_state;

    awaitable<void> run() override { co_return; }

    // The orders channel shows the order resting, as ArcusGateway handles it.
    void ack() { ack_order(order()); }
    void ack(const std::string& side) { ack_order(orders.at(side)); }
    void ack_order(const OrderPtr& o) {
        o->seen_open = true;
        set_state(*o, OrderState::OPEN);
    }
    void fill(const std::string& size) { fill_order(order(), size); }
    void fill(const std::string& side, const std::string& size) { fill_order(orders.at(side), size); }
    void fill_order(const OrderPtr& o, const std::string& size) {
        fill(o, o->filled + Decimal(size));
        if (o->remaining() == 0) finish(o, OrderState::FILLED);
    }
    void confirm_cancel() { finish(order(), OrderState::CANCELED); }
    void cancelled(const std::string& side) { finish(orders.at(side), OrderState::CANCELED); }
    void add_position(const Decimal& delta) { Gateway::add_position(delta, std::nullopt); }
    void set_position(const Decimal& p) { position_ = p; }
    std::map<std::string, std::tuple<Decimal, Decimal, double>>& gone() { return gone_; }

protected:
    void send_place(const OrderPtr& o) override { sent.emplace_back("place", o); }
    void send_cancel(const OrderPtr& o, const std::string& w) override {
        sent.emplace_back("cancel", o);
        why = w;
    }
};

// Connected, but catching up after a reconnect: no placing yet, cancels can go.
class NotReadyGateway : public FakeGateway {
public:
    bool can_cancel() const override { return true; }
};

// The requests made since the last call.
inline std::vector<Action> actions(FakeGateway& gw) {
    std::vector<Action> out;
    for (auto& [kind, o] : gw.sent) out.push_back({kind, o->side, o->price, o->size});
    gw.sent.clear();
    return out;
}

inline Action A(const char* kind, const char* side, const char* price, const char* size) {
    return {kind, side, Decimal(price), Decimal(size)};
}

inline MarketView book(std::optional<double> fair, std::vector<Level> bids, std::vector<Level> asks,
                       const std::string& symbol = "BTC-USD") {
    MarketView v;
    v.venue = std::make_shared<BookTop>(BookTop {"arcus", symbol, std::move(bids), std::move(asks), 1, std::nullopt, 0});
    v.fair_bid = fair;
    return v;
}

inline MarketView view(std::optional<double> fair, double bid = 99'990.0, double ask = 99'990.1) {
    return book(fair, {{bid, 1.0}}, {{ask, 1.0}});
}

struct Rig {
    std::unique_ptr<FakeGateway> gw;
    std::unique_ptr<QuoteStrategy> s;
};

inline StrategyOptions options(double markup, double tol, double order_size) {
    StrategyOptions o;
    o.markup = markup;
    o.tol = tol;
    o.order_size = order_size;
    return o;
}

inline Rig make(double markup = 0.001, double tol = 0.0002, double order_size = 0.001) {
    Rig r;
    r.gw = std::make_unique<FakeGateway>();
    r.s = std::make_unique<QuoteStrategy>(r.gw.get(), btc_spec(), options(markup, tol, order_size));
    return r;
}

// ~$990 orders at ~$100k, and the default limit of $1000 either way: one order fills it
constexpr double SIZE = 0.0099;

// direction "both"; `tweak` sets any other option.
template <class F>
Rig both(double markup, double tol, double order_size, double limit, F&& tweak) {
    StrategyOptions o = options(markup, tol, order_size);
    o.direction = "both";
    o.max_position_usd = limit;
    tweak(o);
    Rig r;
    r.gw = std::make_unique<FakeGateway>();
    r.s = std::make_unique<QuoteStrategy>(r.gw.get(), btc_spec(), o);
    return r;
}

inline Rig both(double markup = 0.001, double tol = 0.0002, double order_size = SIZE, double limit = 1000.0) {
    return both(markup, tol, order_size, limit, [](StrategyOptions&) {});
}

}  // namespace nmm::test

// Each test helper header keeps its names apart (several define a FakeGateway or a view()).
namespace nmm::test {
using namespace nmm::test::strategy;
}  // namespace nmm::test
