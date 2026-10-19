#include "trading/market.hpp"

#include <format>
#include <stdexcept>

#include "net/net.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

Decimal decimal_field(const Json& m, std::string_view key) {
    const Json* v = jget(m, key);
    if (v == nullptr) throw std::invalid_argument(std::format("market field {} missing", key));
    return Decimal::parse(to_str(*v));
}

int64_t exact_units(const Decimal& value, const Decimal& unit, const char* what) {
    try {
        return value.units(unit);
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument(std::format("{} {} is not a multiple of {}", what, value.str(), unit.str()));
    }
}

}  // namespace

MarketSpec MarketSpec::from_json(const Json& m) {
    MarketSpec spec;
    spec.market_id = to_int(*jget(m, "marketId"));
    spec.symbol = to_str(jget(m, "marketDisplayName"));
    spec.tick_size = decimal_field(m, "tickSize");
    if (const Json* tiers = jget(m, "tickTiers"); tiers != nullptr && tiers->is_array()) {
        for (const auto& t : tiers->get_array()) {
            std::optional<Decimal> up_to;
            if (truthy(jget(t, "upToPrice"))) up_to = Decimal::parse(to_str(*jget(t, "upToPrice")));
            spec.tick_tiers.emplace_back(up_to, decimal_field(t, "tick"));
        }
    }
    if (spec.tick_tiers.empty()) spec.tick_tiers.emplace_back(std::nullopt, spec.tick_size);
    spec.step_size = decimal_field(m, "stepSize");
    spec.min_order_size = decimal_field(m, "minOrderSize");
    spec.min_order_notional = decimal_field(m, "minOrderNotional");
    spec.max_order_size = decimal_field(m, "maxOrderSize");
    spec.hours = Hours::from_json(jget(m, "regularTradingHours"));
    if (const Json* rth = jget(m, "isOutsideRth"); rth != nullptr && rth->is_bool()) spec.outside_rth = rth->get_bool();
    return spec;
}

Decimal MarketSpec::tick_at(const Decimal& price) const {
    for (const auto& [up_to, tick] : tick_tiers)
        if (!up_to || price < *up_to) return tick;
    return tick_tiers.back().second;
}

Decimal MarketSpec::floor_price(const Decimal& price) const { return price.floor_to(tick_at(price)); }

Decimal MarketSpec::ceil_price(const Decimal& price) const { return price.ceil_to(tick_at(price)); }

int64_t MarketSpec::price_ticks(const Decimal& price) const { return exact_units(price, tick_size, "price"); }

int64_t MarketSpec::size_quantums(const Decimal& size) const { return exact_units(size, step_size, "size"); }

awaitable<MarketSpec> fetch_market(std::string rest_url, std::string market) {
    std::string url = with_params(std::string(rstrip(rest_url, '/')) + "/v1/markets", {{"market", market}});
    HttpResponse resp = co_await http_get(url, {}, 10.0);
    resp.raise_for_status(url);
    Json data = parse_json(resp.body);
    if (const Json* markets = jget(data, "markets"); markets != nullptr && markets->is_array()) {
        for (const auto& item : markets->get_array()) {
            if (upper(jstr(item, "marketDisplayName").value_or("")) != upper(market)) continue;
            if (jstr(item, "status").value_or("") != "ONLINE")
                throw std::runtime_error(std::format("Arcus market {} is {}, not ONLINE", market, jstr_or(item, "status")));
            co_return MarketSpec::from_json(item);
        }
    }
    throw std::runtime_error(std::format("Arcus market {} not found", market));
}

}  // namespace nmm
