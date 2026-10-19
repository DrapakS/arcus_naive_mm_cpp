#include "trading/signing.hpp"

#include <stdexcept>

#include "util/pyfmt.hpp"

namespace nmm {

namespace {

int side_code(const std::string& side) {
    if (side == "BUY") return 0;
    if (side == "SELL") return 1;
    throw std::invalid_argument("unknown side " + side);
}

int time_in_force_code(const std::string& tif) {
    if (tif == "GTT") return 0;
    if (tif == "FOK") return 1;
    if (tif == "IOC") return 2;
    if (tif == "ALO") return 3;
    throw std::invalid_argument("unknown timeInForce " + tif);
}

}  // namespace

std::string place_order_message(const PlaceOrderFields& f) {
    JsonObject payload {
        {"ad", lower(f.address)},
        {"ai", f.account_index},
        {"ct", f.timestamp_ns},
        {"g", f.good_til_us * 1000},  // signed in nanoseconds, sent in microseconds
        {"m", f.market_id},
        {"op", OP_PLACE},
        {"p", f.price_ticks},
        {"q", f.size_quantums},
        {"r", f.reduce_only ? 1 : 0},
        {"s", side_code(f.side)},
        {"t", time_in_force_code(f.time_in_force)},
        {"v", 1},
    };
    if (f.client_id && !f.client_id->empty()) payload["c"] = *f.client_id;
    return canonical_json(payload);
}

std::string cancel_order_message(const std::string& address, int account_index, int64_t market_id, int64_t timestamp_ns,
                                 std::optional<std::string> client_id, std::optional<std::string> order_id) {
    if (client_id.has_value() == order_id.has_value())
        throw std::invalid_argument("cancel needs exactly one of client_id / order_id");
    JsonObject payload {
        {"ad", lower(address)}, {"ai", account_index}, {"ct", timestamp_ns}, {"m", market_id}, {"op", OP_CANCEL}, {"v", 1},
    };
    if (client_id) payload["c"] = *client_id;
    else payload["id"] = *order_id;
    return canonical_json(payload);
}

std::string legacy_message(int64_t timestamp_ns, const std::string& action, const Json& payload) {
    return std::to_string(timestamp_ns) + action + canonical_json(payload);
}

}  // namespace nmm
