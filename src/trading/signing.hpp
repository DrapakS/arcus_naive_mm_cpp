// Arcus request signing (docs.arcus.xyz, "Authentication").
//
// Scheme 1, for placeOrder/cancelOrder/modifyOrder: the signed message is a
// compact, key-sorted JSON object of engine-native integers (price in ticks,
// size in quantums), not the request body.
//
// Scheme 2, for everything else signed (cancelAllOrders, scheduleCancel, ...):
// timestamp + action + canonical_json(payload).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "util/json.hpp"

namespace nmm {

constexpr int OP_PLACE = 1, OP_CANCEL = 2;

struct PlaceOrderFields {
    std::string address;
    int account_index = 0;
    int64_t market_id = 0;
    std::string side;  // BUY | SELL
    int64_t price_ticks = 0;
    int64_t size_quantums = 0;
    std::string time_in_force;  // GTT | FOK | IOC | ALO
    int64_t good_til_us = 0;
    bool reduce_only = false;
    std::optional<std::string> client_id;
    int64_t timestamp_ns = 0;
};

std::string place_order_message(const PlaceOrderFields& f);

// Exactly one of client_id / order_id; throws std::invalid_argument otherwise.
std::string cancel_order_message(const std::string& address, int account_index, int64_t market_id, int64_t timestamp_ns,
                                 std::optional<std::string> client_id = {}, std::optional<std::string> order_id = {});

std::string legacy_message(int64_t timestamp_ns, const std::string& action, const Json& payload);

}  // namespace nmm
