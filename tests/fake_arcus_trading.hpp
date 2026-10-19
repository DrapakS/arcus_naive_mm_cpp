// Arcus order entry over /v1/ws (FakeArcusTrading of tests/fakes.py): verifies
// every signature like the real server would, keeps orders and the position, and
// pushes orders / positions updates to the connections subscribed to them;
// replies go back on the connection a request came in on. Tests fill orders with
// fill, can hold placements back (hold_places) so a cancel overtakes them, let
// them land right behind a cancel that found nothing (land_held_on_miss), with or
// without an orders event (quiet_landing), and can delay the event of a fill
// (fill(..., push = false)). A fill is published on the public trades channel
// first, like the live exchange, unless public_ = false.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fake_server.hpp"
#include "util/decimal.hpp"
#include "util/json.hpp"

namespace nmm::test {

using OrderRow = std::tuple<std::string, std::string, std::string>;  // (side, price, size)

class FakeArcusTrading : public FakeServer {
public:
    FakeArcusTrading(boost::asio::io_context& ioc, std::string address, int account_index, std::string public_key,
                     std::vector<OrderRow> open_orders = {}, std::vector<OrderRow> closed_orders = {});

    std::string ws_url() const { return "ws://" + base() + "/v1/ws"; }
    std::string rest_url() const { return "http://" + base(); }

    std::vector<JsonObject*> open_orders();
    JsonObject& new_order(const std::string& client_id, const std::string& side, const std::string& price, const std::string& size);
    JsonObject& order_by_client_id(const std::string& client_id);
    void push_order(const std::string& client_id);
    // A fill of our order: in the public trades (`public_`), then on userFills (`user_fills`),
    // then on the orders channel (`push`), under one tradeId.
    void fill(const std::string& client_id, const std::string& size, bool push = true, bool public_ = true,
              bool user_fills = false, std::optional<std::string> trade_id = std::nullopt);
    // A public trade with `order` as the maker (or `maker_id`, someone else's).
    void publish_trade(const JsonObject& order, const std::string& size, std::optional<std::string> maker_id = std::nullopt,
                       std::optional<std::string> trade_id = std::nullopt);
    // One taker order in one message, as Arcus sends it: trades with others at the prices
    // `before` ours, then ours, then the prices `after`.
    void publish_sweep(const JsonObject& order, const std::string& size, std::vector<std::string> before = {},
                       std::vector<std::string> after = {}, const std::string& taker = "sweeper");
    void release_places();
    // A frame of our subaccount's channels, and to every subscriber.
    std::string frame(const std::string& kind, const std::string& channel, const Json& contents) const;
    void broadcast(const std::string& text);

    std::string address;
    int account_index;
    std::string public_key;
    std::map<std::string, JsonObject> orders;  // orderId -> order
    Decimal position;
    int64_t seq = 100;
    std::vector<std::pair<std::string, Json>> posts;  // (method, body)
    std::vector<std::optional<int64_t>> dms;
    int cancel_pool = 40'000;
    std::optional<std::pair<int, Json>> cancel_error;  // answer cancels with this error instead
    bool mute_cancels = false;  // take cancels without answering or acting on them
    bool mute_dms = false;  // take switch requests without answering them
    bool quiet_orders = false;  // no orders-channel events (as when they are lost with a connection)
    bool hold_places = false;  // acknowledge placements but book them only on release_places()
    bool lose_places = false;  // take placements without answering or booking them (lost on the way)
    bool ack_unknown_cancels = false;  // answer 202 to a cancel that finds nothing, as Arcus does
    bool land_held_on_miss = false;  // a cancel that finds nothing lets held placements land right after
    bool quiet_landing = false;  // held placements land without an orders event
    std::optional<double> max_cancel_age_ms;  // refuse cancels signed longer ago than this
    std::vector<Json> held;
    std::set<int64_t> timestamps;  // like Arcus, each signed timestamp is taken once
    double cancel_event_delay = 0.0;  // seconds the CANCELED event trails the cancel's reply
    std::vector<std::shared_ptr<ServerWs>> subscribers, trade_subscribers;
    int trade_ids = 0;
    int connections_made = 0;
    std::shared_ptr<ServerWs> ws;  // the connection tests drop to cut the gateway's main link
    std::optional<int> open_orders_error;  // answer GET /v1/openOrders with this HTTP status
    std::optional<Json> open_orders_body;  // answer GET /v1/openOrders with this body instead
    int open_orders_requests = 0;
    // A stale orders snapshot, as Arcus has sent after reconnects: orders it lists as OPEN that it
    // no longer has, filled sizes it shows lower than they are, orders of ours it leaves out.
    std::vector<JsonObject> stale_open;
    std::map<std::string, std::string> snapshot_filled;  // clientId -> filledSize shown in the snapshot
    std::set<std::string> snapshot_hide;  // clientIds left out of the snapshot

private:
    void push(JsonObject& order);
    bool verify(const Json& request, const std::string& message) const;
    void on_post(const std::shared_ptr<ServerWs>& conn, int64_t request_id, const Json& request);
    void on_frame(const std::shared_ptr<ServerWs>& conn, const std::string& text);
    std::map<const ServerWs*, int64_t> last_ids_;
};

}  // namespace nmm::test
