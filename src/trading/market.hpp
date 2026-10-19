// Arcus market metadata and exact price/size arithmetic.
//
// Prices and sizes that go to the exchange are Decimal: they must be exact
// multiples of the tick and step, and the signed payload carries them as integer
// ticks (price / tickSize) and quantums (size / stepSize).
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "trading/session.hpp"
#include "util/aio.hpp"
#include "util/decimal.hpp"
#include "util/json.hpp"

namespace nmm {

struct MarketSpec {
    int64_t market_id = 0;
    std::string symbol;
    Decimal tick_size;  // base tick; the signing unit at every price
    std::vector<std::pair<std::optional<Decimal>, Decimal>> tick_tiers;  // (up_to_price or none, tick), ascending
    Decimal step_size;
    Decimal min_order_size;
    Decimal min_order_notional;
    Decimal max_order_size;
    std::optional<Hours> hours;  // regularTradingHours: the session of a market with hours (stocks, ETFs)
    std::optional<bool> outside_rth;  // isOutsideRth when the spec was read

    static MarketSpec from_json(const Json& m);

    // Price increment allowed in the band `price` falls in.
    Decimal tick_at(const Decimal& price) const;
    Decimal floor_price(const Decimal& price) const;
    Decimal floor_price(double price) const { return floor_price(Decimal::from_double(price)); }
    Decimal ceil_price(const Decimal& price) const;
    Decimal ceil_price(double price) const { return ceil_price(Decimal::from_double(price)); }
    Decimal floor_size(const Decimal& size) const { return size.floor_to(step_size); }
    int64_t price_ticks(const Decimal& price) const;
    int64_t size_quantums(const Decimal& size) const;
};

// GET /v1/markets: the spec of `market`; throws when it is missing or not ONLINE.
awaitable<MarketSpec> fetch_market(std::string rest_url, std::string market);

}  // namespace nmm
