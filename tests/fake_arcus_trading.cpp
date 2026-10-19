#include "fake_arcus_trading.hpp"

#include <openssl/evp.h>

#include <format>
#include <stdexcept>

#include "util/aio.hpp"
#include "util/clock.hpp"
#include "util/pyfmt.hpp"

namespace nmm::test {

namespace {

std::vector<unsigned char> from_hex(const std::string& hex) {
    std::vector<unsigned char> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<unsigned char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

std::string str(const Json* v) { return v == nullptr ? "" : to_str(*v); }

}  // namespace

FakeArcusTrading::FakeArcusTrading(boost::asio::io_context& ioc, std::string address_, int account_index_, std::string public_key_,
                                   std::vector<OrderRow> open_orders_, std::vector<OrderRow> closed_orders)
    : FakeServer(ioc), address(lower(address_)), account_index(account_index_), public_key(std::move(public_key_)) {
    for (size_t i = 0; i < open_orders_.size(); ++i) {
        auto& [side, price, size] = open_orders_[i];
        new_order(std::format("old{}", i), side, price, size);
    }
    for (size_t i = 0; i < closed_orders.size(); ++i) {
        auto& [side, price, size] = closed_orders[i];
        JsonObject& o = new_order(std::format("closed{}", i), side, price, size);
        o["status"] = "FILLED";
        o["state"] = "FILLED";
        o["filledSize"] = size;
        o["remainingSize"] = "0";
    }
    on_ws("/v1/ws", [this](std::shared_ptr<ServerWs> conn) {
        connections_made += 1;
        conn->send(dump_json(JsonObject {{"type", "connected"}, {"connection_id", std::to_string(connections_made)}}));
        std::weak_ptr<ServerWs> weak = conn;
        conn->on_message = [this, weak](const std::string& text) {
            if (auto c = weak.lock()) on_frame(c, text);
        };
        const ServerWs* raw = conn.get();
        conn->on_close = [this, weak, raw] {
            last_ids_.erase(raw);
            auto c = weak.lock();
            std::erase_if(subscribers, [&](const auto& s) { return s == c; });
            std::erase_if(trade_subscribers, [&](const auto& s) { return s == c; });
        };
    });
    on_get("/v1/openOrders", [this](const HttpRequest& request) {
        open_orders_requests += 1;
        if (open_orders_error) return HttpReply(*open_orders_error, R"({"error": "unavailable"})");
        if (open_orders_body) return HttpReply(200, dump_json(*open_orders_body));
        auto address_q = request.query.find("address");
        auto key = request.headers.find("x-api-key");
        if (address_q == request.query.end() || address_q->second != address || key == request.headers.end() || key->second != public_key)
            return HttpReply(403, R"({"error": "address not on access whitelist"})");
        JsonArray listed;
        for (JsonObject* o : open_orders()) {
            auto market = request.query.find("market");
            std::string id = to_str(o->at("marketId"));
            if (market == request.query.end() || market->second == id) listed.push_back(*o);
        }
        size_t n = listed.size();
        return HttpReply(200, dump_json(JsonObject {{"orders", std::move(listed)}, {"total", n}}));
    });
}

std::vector<JsonObject*> FakeArcusTrading::open_orders() {
    std::vector<JsonObject*> out;
    for (auto& [id, o] : orders)
        if (to_str(o.at("status")) == "OPEN") out.push_back(&o);
    return out;
}

JsonObject& FakeArcusTrading::new_order(const std::string& client_id, const std::string& side, const std::string& price,
                                        const std::string& size) {
    std::string order_id = std::format("{:016x}", orders.size() + 1);
    orders[order_id] = JsonObject {
        {"orderId", order_id}, {"clientId", client_id}, {"marketId", 1}, {"marketDisplayName", "BTC-USD"},
        {"side", side}, {"type", "LIMIT"}, {"status", "OPEN"}, {"state", "OPEN"}, {"price", price},
        {"originalSize", size}, {"filledSize", "0"}, {"remainingSize", size}, {"timeInForce", "ALO"},
    };
    return orders[order_id];
}

JsonObject& FakeArcusTrading::order_by_client_id(const std::string& client_id) {
    for (auto& [id, o] : orders)
        if (to_str(o.at("clientId")) == client_id) return o;
    throw std::out_of_range("no order " + client_id);
}

std::string FakeArcusTrading::frame(const std::string& kind, const std::string& channel, const Json& contents) const {
    return dump_json(JsonObject {{"type", kind}, {"channel", channel}, {"id", address}, {"accountIndex", account_index},
                                 {"market", "BTC-USD"}, {"contents", contents}});
}

void FakeArcusTrading::broadcast(const std::string& text) {
    for (auto& s : std::vector(subscribers))
        if (!s->closed()) s->send(text);
}

void FakeArcusTrading::push(const JsonObject& order) {
    if (quiet_orders) return;
    seq += 1;
    JsonObject event = order;
    event["sequenceNumber"] = seq;
    event["updatedAt"] = time_ns() / 1000;  // like Arcus: µs
    broadcast(frame("channel_data", "orders", event));
}

void FakeArcusTrading::push_order(const std::string& client_id) { push(order_by_client_id(client_id)); }

void FakeArcusTrading::fill(const std::string& client_id, const std::string& size, bool push_event, bool public_, bool user_fills,
                            std::optional<std::string> trade_id) {
    JsonObject& order = order_by_client_id(client_id);
    if (public_) publish_trade(order, size, std::nullopt, trade_id);
    Decimal filled = Decimal::parse(str(order.if_contains("filledSize"))) + Decimal::parse(size);
    Decimal remaining = Decimal::parse(str(order.if_contains("originalSize"))) - filled;
    std::string side = str(order.if_contains("side"));
    if (user_fills) {  // the shape Arcus sends (seen live)
        if (!public_ && !trade_id) trade_ids += 1;
        JsonObject f {
            {"tradeId", trade_id.value_or(std::to_string(trade_ids))}, {"orderId", order.at("orderId")}, {"marketId", 1},
            {"marketDisplayName", "BTC-USD"}, {"clientId", client_id}, {"side", side}, {"originalSize", order.at("originalSize")},
            {"size", size}, {"price", order.at("price")}, {"fee", "0"}, {"closedPnl", "0"}, {"role", "MAKER"},
            {"remainingSize", remaining.str()}, {"positionEffect", side == "BUY" ? "OPEN_LONG" : "OPEN_SHORT"},
            {"createdAt", time_ns() / 1000}, {"sequenceNumber", seq + 1},
        };
        broadcast(frame("channel_data", "userFills", JsonObject {{"isSnapshot", false}, {"fills", JsonArray {f}}}));
    }
    order["filledSize"] = filled.str();
    order["remainingSize"] = remaining.str();
    order["status"] = remaining == 0 ? "FILLED" : "OPEN";
    order["state"] = remaining == 0 ? "FILLED" : "PARTIALLY_FILLED";
    position += side == "BUY" ? Decimal::parse(size) : -Decimal::parse(size);
    if (!push_event) return;
    push(order);
    seq += 1;
    std::string pos_side = !position ? "FLAT" : position > 0 ? "LONG" : "SHORT";
    // like Arcus: a streaming update is one flat position, without a sequence id
    broadcast(frame("channel_data", "positions",
                    JsonObject {{"isSnapshot", false}, {"marketId", 1}, {"marketDisplayName", "BTC-USD"}, {"side", pos_side},
                                {"size", position.str()}, {"averageEntryPrice", "99900"}}));
}

void FakeArcusTrading::publish_trade(const JsonObject& order, const std::string& size, std::optional<std::string> maker_id,
                                     std::optional<std::string> trade_id) {
    trade_ids += 1;
    JsonObject trade {
        {"marketDisplayName", "BTC-USD"}, {"marketId", 1}, {"side", str(order.if_contains("side")) == "BUY" ? "SELL" : "BUY"},
        {"price", order.at("price")}, {"size", size}, {"tradeId", trade_id.value_or(std::to_string(trade_ids))},
        {"timestamp", time_ns() / 1000}, {"makerOrderId", maker_id ? Json(*maker_id) : order.at("orderId")},
        {"takerOrderId", "taker"}, {"makerAddress", maker_id ? std::string("0xsomeoneelse") : address},
        {"takerAddress", "0xtaker"}, {"sequenceNumber", seq},
    };
    std::string text = dump_json(JsonObject {{"type", "channel_data"}, {"channel", "trades"}, {"id", "BTC-USD"}, {"contents", JsonArray {trade}}});
    for (auto& s : std::vector(trade_subscribers))
        if (!s->closed()) s->send(text);
}

void FakeArcusTrading::publish_sweep(const JsonObject& order, const std::string& size, std::vector<std::string> before,
                                     std::vector<std::string> after, const std::string& taker) {
    JsonArray trades;
    std::vector<std::pair<std::string, bool>> prices;
    for (auto& p : before) prices.emplace_back(p, false);
    prices.emplace_back(str(order.if_contains("price")), true);
    for (auto& p : after) prices.emplace_back(p, false);
    for (auto& [price, mine] : prices) {
        trade_ids += 1;
        trades.push_back(JsonObject {
            {"marketDisplayName", "BTC-USD"}, {"marketId", 1}, {"side", str(order.if_contains("side")) == "BUY" ? "SELL" : "BUY"},
            {"price", price}, {"size", mine ? size : std::string("0.002")}, {"tradeId", std::to_string(trade_ids)},
            {"timestamp", time_ns() / 1000}, {"makerOrderId", mine ? order.at("orderId") : Json("other" + price)},
            {"takerOrderId", taker}, {"makerAddress", mine ? address : std::string("0xsomeoneelse")}, {"takerAddress", "0xtaker"},
            {"sequenceNumber", seq},
        });
    }
    std::string text = dump_json(JsonObject {{"type", "channel_data"}, {"channel", "trades"}, {"id", "BTC-USD"}, {"contents", trades}});
    for (auto& s : std::vector(trade_subscribers))
        if (!s->closed()) s->send(text);
}

void FakeArcusTrading::release_places() {
    hold_places = false;
    auto landing = std::move(held);
    held.clear();
    for (const Json& body : landing) {
        JsonObject& order = new_order(str(jget(body, "clientId")), str(jget(body, "orderSide")), str(jget(body, "price")),
                                      str(jget(body, "quantity")));
        if (!quiet_landing) push(order);
    }
}

bool FakeArcusTrading::verify(const Json& request, const std::string& message) const {
    auto pub = from_hex(public_key);
    std::string sig_hex = str(jget(request, "signature"));
    if (sig_hex.size() != 128 || sig_hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    auto sig = from_hex(sig_hex);
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
              EVP_DigestVerify(ctx, sig.data(), sig.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

void FakeArcusTrading::on_post(const std::shared_ptr<ServerWs>& conn, int64_t request_id, const Json& request) {
    std::string method = str(jget(request, "type"));
    const Json& body = *jget(request, "payload");
    int64_t ts = to_int(*jget(request, "timestamp"));
    posts.emplace_back(method, body);
    auto reply = [&](JsonObject extra) {
        JsonObject out {{"method", method}, {"id", request_id}};
        for (auto& kv : extra) out[kv.key()] = kv.value();
        conn->send(dump_json(out));
    };
    if (method == "scheduleCancel") {
        if (mute_dms) return;
        bool ok = verify(request, std::to_string(ts) + "scheduleCancel" + canonical_json(body));
        if (ok) {
            const Json* time = jget(body, "time");
            dms.push_back(time != nullptr ? std::optional(to_int(*time)) : std::nullopt);
        }
        reply({{"status", ok ? 200 : 401}, {"result", JsonObject {{"status", "OK"}}}});
        return;
    }
    JsonObject payload {{"ad", lower(str(jget(body, "address")))}, {"ai", jget(body, "accountIndex") ? *jget(body, "accountIndex") : Json()},
                        {"ct", ts}, {"m", jget(body, "marketId") ? *jget(body, "marketId") : Json()}, {"v", 1}};
    const Json* body_ts = jget(body, "timestamp");
    if (body_ts == nullptr || !body_ts->is_int64() || body_ts->get_int64() != ts) payload["ct"] = -1;  // body and envelope timestamps must agree
    if (method == "placeOrder") {
        payload["op"] = 1;
        payload["c"] = *jget(body, "clientId");
        payload["g"] = to_int(*jget(body, "goodTilTime")) * 1000;
        payload["p"] = (Decimal::parse(str(jget(body, "price"))) / Decimal("0.1")).round_int();
        payload["q"] = (Decimal::parse(str(jget(body, "quantity"))) / Decimal("0.00000001")).round_int();
        payload["r"] = truthy(jget(body, "reduceOnly")) ? 1 : 0;
        payload["s"] = str(jget(body, "orderSide")) == "BUY" ? 0 : 1;
        std::string tif = str(jget(body, "timeInForce"));
        payload["t"] = tif == "GTT" ? 0 : tif == "FOK" ? 1 : tif == "IOC" ? 2 : 3;
    } else {
        payload["op"] = 2;
        if (str(jget(body, "kind")) == "clientId") payload["c"] = *jget(body, "clientId");
        else payload["id"] = *jget(body, "orderId");
    }
    if (!verify(request, canonical_json(payload))) {
        reply({{"status", 401}, {"error", JsonObject {{"message", "invalid signature"}}}});
        return;
    }
    if (timestamps.count(ts)) {
        reply({{"status", 401}, {"error", JsonObject {{"type", "TimestampReused"}, {"message", "request timestamp already used"}}}});
        return;
    }
    timestamps.insert(ts);
    if (method == "placeOrder") {
        if (str(jget(body, "orderType")) != "LIMIT" || str(jget(body, "timeInForce")) != "ALO")
            throw std::runtime_error("not an ALO limit order");
        if (lose_places) return;
        if (hold_places) {  // still on its way to the book; a cancel can overtake it
            held.push_back(body);
            reply({{"status", 202}, {"result", JsonObject {{"status", "ACK"}}}});
            return;
        }
        JsonObject& order = new_order(str(jget(body, "clientId")), str(jget(body, "orderSide")), str(jget(body, "price")),
                                      str(jget(body, "quantity")));
        reply({{"status", 202}, {"result", JsonObject {{"orderId", order.at("orderId")}, {"status", "ACK"}}}});
        push(order);
        return;
    }
    if (mute_cancels) return;
    if (max_cancel_age_ms && static_cast<double>(time_ns() - ts) / 1e6 > *max_cancel_age_ms) {
        reply({{"status", 400}, {"error", JsonObject {{"type", "INVALID_TIMESTAMP"}, {"message", "timestamp too old"}}}});
        return;
    }
    if (cancel_error) {
        reply({{"status", cancel_error->first}, {"error", cancel_error->second}});
        return;
    }
    std::string key = str(jget(body, "kind")) == "clientId" ? "clientId" : "orderId";
    std::string wanted = str(jget(body, key));
    JsonObject* order = nullptr;
    for (auto& [id, o] : orders)
        if (str(o.if_contains(key)) == wanted && str(o.if_contains("status")) == "OPEN") {
            order = &o;
            break;
        }
    if (order == nullptr && ack_unknown_cancels) {
        reply({{"status", 202}, {"result", JsonObject {{key, wanted}, {"status", "CANCEL_ACKNOWLEDGED"}}}});
        return;
    }
    if (order == nullptr) {
        reply({{"status", 404}, {"error", JsonObject {{"type", "ORDER_NOT_FOUND"}, {"message", "order not found"}}}});
        if (land_held_on_miss) release_places();
        return;
    }
    cancel_pool -= 1;
    JsonObject result {{"address", address}, {"accountIndex", account_index}, {"marketId", 1}, {"marketDisplayName", "BTC-USD"},
                       {key, wanted}, {"status", "CANCEL_ACKNOWLEDGED"}, {"updateTime", time_ns() / 1000},
                       {"rateLimit", JsonObject {{"pool", "cancel"}, {"remaining", cancel_pool}}}};
    reply({{"status", 202}, {"result", result}});
    (*order)["status"] = "CANCELED";
    (*order)["state"] = "CANCELED";
    if (cancel_event_delay > 0) {
        std::string order_id = str(order->if_contains("orderId"));
        call_later(cancel_event_delay, [this, order_id] {
            if (auto it = orders.find(order_id); it != orders.end()) push(it->second);
        });
    } else {
        push(*order);
    }
}

void FakeArcusTrading::on_frame(const std::shared_ptr<ServerWs>& conn, const std::string& text) {
    Json data = parse_json(text);
    std::string type = str(jget(data, "type")), channel = str(jget(data, "channel"));
    if (type == "subscribe" && channel == "trades") {  // public
        trade_subscribers.push_back(conn);
        conn->send(dump_json(JsonObject {{"type", "subscribed"}, {"channel", "trades"}, {"id", "BTC-USD"}, {"contents", JsonArray {}}}));
    } else if (type == "subscribe") {
        if (str(jget(data, "id")) != address || to_int(*jget(data, "accountIndex")) != account_index)
            throw std::runtime_error("subscribed to someone else's channels");
        ws = conn;
        if (std::find(subscribers.begin(), subscribers.end(), conn) == subscribers.end()) subscribers.push_back(conn);
        Json contents;
        if (channel == "orders") {  // like Arcus: open and recently closed orders apart
            JsonArray open, closed;
            for (auto& [id, o] : orders) {
                std::string client_id = str(o.if_contains("clientId"));
                if (snapshot_hide.count(client_id)) continue;
                JsonObject shown = o;
                if (auto it = snapshot_filled.find(client_id); it != snapshot_filled.end()) shown["filledSize"] = it->second;
                (str(o.if_contains("status")) == "OPEN" ? open : closed).push_back(shown);
            }
            for (auto& o : stale_open) open.push_back(o);
            contents = JsonObject {{"isSnapshot", true}, {"lastSequenceId", seq}, {"openOrders", open}, {"recentClosedOrders", closed}};
        } else {
            JsonObject pos;
            if (position)
                pos["1"] = JsonObject {{"marketId", 1}, {"side", position > 0 ? "LONG" : "SHORT"}, {"size", position.str()},
                                       {"averageEntryPrice", "99900"}};
            contents = JsonObject {{"isSnapshot", true}, {"lastSequenceId", seq}, {"positions", pos}};
        }
        conn->send(frame("subscribed", channel, contents));
    } else if (type == "post") {
        int64_t id = to_int(*jget(data, "id"));
        int64_t& last = last_ids_[conn.get()];
        if (id <= last) {
            conn->send(R"({"type": "error", "message": "request id must increase"})");
            return;
        }
        last = id;
        on_post(conn, id, *jget(data, "request"));
    }
}

}  // namespace nmm::test
