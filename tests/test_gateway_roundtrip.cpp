// Ported from the gateway-level tests of tests/test_roundtrip.py: round trips and the
// position cross-check fed by the live gateway's channels.
#include <gtest/gtest.h>

#include "gateway_rig.hpp"

using namespace nmm;
using namespace nmm::test;
using D = Decimal;
using Access = ArcusGatewayTestAccess;

namespace {

std::unique_ptr<ArcusGateway> offline(const MarketSpec& market) {
    ArcusGateway::Options o;
    o.ws_url = "ws://unused";
    return std::make_unique<ArcusGateway>(market, ArcusCredentials::make(ADDRESS, 1, SECRET), o);
}

OrderPtr order(const std::string& client_id, const std::string& side, const D& price, const D& size) {
    auto o = std::make_shared<Order>(client_id, side, price, size);
    o->order_id = "id-" + client_id;
    return o;
}

std::vector<std::string> starting(const std::vector<std::string>& lines, const std::string& prefix) {
    std::vector<std::string> out;
    for (auto& l : lines)
        if (starts_with(l, prefix)) out.push_back(l);
    return out;
}

}  // namespace

TEST(GatewayRoundTrip, RoundTripsFollowTheExchangePositionThroughAReconnect) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    GatewayLog log;
    auto gw = offline(btc_market());
    Access::on_positions(*gw, parse_json(R"({"isSnapshot": true, "lastSequenceId": 10, "positions": []})"));
    auto filled = [&](const std::string& client_id, const std::string& side, const std::string& price, int64_t seq) {
        gw->set_order(order(client_id, side, D(price), D("0.001")));
        Access::on_order_update(*gw, JsonObject {{"marketId", 1}, {"clientId", client_id}, {"orderId", "id-" + client_id},
                                                 {"status", "FILLED"}, {"filledSize", "0.001"}, {"sequenceNumber", seq}});
    };
    filled("b1", "BUY", "100000", 11);
    EXPECT_EQ(gw->round_trips->position(), D("0.001"));
    EXPECT_EQ(gw->position(), D("0.001"));
    // the main link drops; the sell fills meanwhile; the snapshot after the reconnect is flat
    Access::on_positions(*gw, parse_json(R"({"isSnapshot": true, "lastSequenceId": 20, "positions": []})"));
    EXPECT_EQ(gw->position(), 0);
    EXPECT_EQ(gw->round_trips->position(), 0);
    EXPECT_FALSE(gw->round_trips->open());
    filled("b2", "BUY", "100000", 21);
    filled("s2", "SELL", "100010", 22);
    auto trips = starting(log.lines(), "=== ROUND TRIP");
    ASSERT_FALSE(trips.empty());
    EXPECT_TRUE(starts_with(trips.back(), "=== ROUND TRIP #1 long 0.001: bought @ 100000.00, sold @ 100010.00")) << trips.back();
    // a fill the reconnect snapshot already counted, whose orders event comes after it
    Access::on_positions(*gw, parse_json(R"({"isSnapshot": true, "lastSequenceId": 30, "positions": [
        {"marketId": 1, "side": "LONG", "size": "0.001"}]})"));
    filled("b3", "BUY", "100000", 29);  // seq 29 <= 30: in the snapshot already, not counted twice
    EXPECT_EQ(gw->position(), D("0.001"));
    EXPECT_EQ(gw->round_trips->position(), D("0.001"));
}

TEST(GatewayRoundTrip, StreamingPositionUpdatesAreCheckedOnceTheySettle) {
    Patch settle(POSITION_SETTLE_S, 0.01);
    boost::asio::io_context ioc;
    set_loop(ioc);
    GatewayLog log;
    auto gw = offline(btc_market());
    Access::on_positions(*gw, parse_json(R"({"isSnapshot": true, "lastSequenceId": 10, "positions": {}})"));
    gw->set_order(order("b1", "BUY", D(100000), D("0.001")));
    auto update = [&](const std::string& side, const std::string& size) {  // the flat shape Arcus streams, no sequence id
        Access::on_positions(*gw, JsonObject {{"isSnapshot", false}, {"marketId", 1}, {"marketDisplayName", "BTC-USD"},
                                              {"side", side}, {"size", size}, {"averageEntryPrice", "100000"}});
    };
    auto warned = [&] { return starting(log.lines(), "!!! exchange reports"); };
    update("LONG", "0.001");  // the position update can come before the orders channel tells of the fill
    Access::on_order_update(*gw, JsonObject {{"marketId", 1}, {"clientId", "b1"}, {"orderId", "id-b1"}, {"status", "FILLED"},
                                             {"filledSize", "0.001"}, {"sequenceNumber", 11}});
    run_for(ioc, 0.05);
    EXPECT_TRUE(warned().empty());  // it settled in agreement
    update("LONG", "0.002");
    update("LONG", "0.002");
    run_for(ioc, 0.05);
    update("FLAT", "0");
    run_for(ioc, 0.05);
    EXPECT_EQ(warned(), (std::vector<std::string> {"!!! exchange reports position 0.002, tracked 0.001",
                                                   "!!! exchange reports position 0, tracked 0.001"}));
}

TEST(GatewayRoundTrip, AReconnectSnapshotRestartsThePositionCrossCheck) {
    Patch settle(POSITION_SETTLE_S, 0.02);
    boost::asio::io_context ioc;
    set_loop(ioc);
    GatewayLog log;
    auto gw = offline(btc_market());
    Access::on_positions(*gw, parse_json(R"({"isSnapshot": true, "lastSequenceId": 10, "positions": {}})"));
    Access::on_positions(*gw, parse_json(R"({"isSnapshot": false, "marketId": 1, "side": "LONG", "size": "0.002"})"));  // then the link drops
    Access::on_positions(*gw, parse_json(R"({"isSnapshot": true, "lastSequenceId": 30, "positions": {}})"));  // reconnected: flat
    run_for(ioc, 0.06);
    EXPECT_TRUE(starting(log.lines(), "!!! exchange reports").empty());
}

TEST(GatewayRoundTrip, AnExitCancelledUnfilledMakesNoZeroSizeTrip) {
    // ETH: 0.0015 found at start and set aside, 0.0006 of a buy filled, so the whole 0.0021 is sold.
    // The first sell was re-placed without filling, and settling it ended the 0.0006 trip as "ROUND
    // TRIP long 0" and handed its buy to the set-aside, priced at the old entry.
    boost::asio::io_context ioc;
    set_loop(ioc);
    MarketSpec eth = MarketSpec::from_json(parse_json(R"({
        "marketId": 2, "marketDisplayName": "ETH-USD", "tickSize": "0.01", "stepSize": "0.0000001",
        "minOrderSize": "0.001", "minOrderNotional": "5", "maxOrderSize": "10000"})"));
    GatewayLog log;
    auto gw = offline(eth);
    Access::on_positions(*gw, parse_json(R"({"isSnapshot": true, "lastSequenceId": 10, "positions": [
        {"marketId": 2, "side": "LONG", "size": "0.0015", "averageEntryPrice": "2400"}]})"));
    int64_t seq = 11;
    auto event = [&](const OrderPtr& o, const std::string& status, const std::string& filled) {
        Access::on_order_update(*gw, JsonObject {{"marketId", 2}, {"clientId", o->client_id}, {"orderId", *o->order_id},
                                                 {"status", status}, {"filledSize", filled}, {"sequenceNumber", seq++}});
    };
    OrderPtr buy = order("b1", "BUY", D("2499.96"), D("0.041"));
    gw->set_order(buy);
    event(buy, "OPEN", "0.0006");
    event(buy, "CANCELED", "0.0006");
    OrderPtr first = order("s1", "SELL", D("2500.03"), D("0.0021"));
    gw->set_order(first);
    event(first, "OPEN", "0");
    event(first, "CANCELED", "0");  // re-placed, nothing of it filled
    OrderPtr second = order("s2", "SELL", D("2500.10"), D("0.0021"));
    gw->set_order(second);
    event(second, "FILLED", "0.0021");
    auto trips = starting(log.lines(), "=== ROUND TRIP");
    ASSERT_EQ(trips.size(), 1u) << log.text();
    EXPECT_TRUE(starts_with(trips[0], "=== ROUND TRIP #1 long 0.0006: bought @ 2499.960, sold @ 2500.100")) << trips[0];
    EXPECT_TRUE(contains(log.lines(), "=== SET-ASIDE position closed: sold 0.0015 @ 2500.100, entry 2400: +417.08 bp ($+0.1502)"))
        << log.text();
}

TEST(GatewayRoundTrip, AShortReadsAsAShortWhetherItsSizeIsSignedOrNot) {
    Patch settle(POSITION_SETTLE_S, 0.01);
    struct Case {
        const char* side;
        const char* size;
        const char* position;
    };
    for (const Case& c : {Case {"SHORT", "-0.001", "-0.001"},  // signed, as Arcus sends it since Oct 2026: once read as a long
                          Case {"SHORT", "0.001", "-0.001"},  // unsigned, as it used to
                          Case {"LONG", "0.001", "0.001"}, Case {"FLAT", "0", "0"}}) {
        boost::asio::io_context ioc;
        set_loop(ioc);
        GatewayLog log;
        auto gw = offline(btc_market());
        JsonObject row {{"marketId", 1}, {"marketDisplayName", "BTC-USD"}, {"side", c.side}, {"size", c.size}};
        Access::on_positions(*gw, JsonObject {{"isSnapshot", true}, {"lastSequenceId", 10}, {"positions", JsonObject {{"1", row}}}});  // (re)connecting
        EXPECT_EQ(gw->position(), D(c.position)) << c.side << " " << c.size;
        JsonObject streaming = row;
        streaming["isSnapshot"] = false;
        Access::on_positions(*gw, streaming);  // a streaming update agrees
        run_for(ioc, 0.05);
        EXPECT_TRUE(starting(log.lines(), "!!! exchange reports").empty()) << c.side << " " << c.size;
    }
}
