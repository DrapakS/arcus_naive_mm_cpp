// Ported from tests/test_trading_gateway.py (and the paper gateway tests of
// tests/test_both_sides.py): the live gateway against the fake exchange.
#include <gtest/gtest.h>

#include "gateway_rig.hpp"
#include "trading/paper.hpp"

using namespace nmm;
using namespace nmm::test;
using D = Decimal;

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

TEST(TradingGateway, BuyPartialFillThenSellThenBuyWithSignedRequests) {
    Rig rig;
    ASSERT_FALSE(rig.fake->dms.empty());
    EXPECT_TRUE(rig.fake->dms[0].has_value());  // switch armed with a valid signature
    rig.quote();
    OrderPtr buy = rig.gw->order();
    ASSERT_TRUE(buy);
    EXPECT_EQ(buy->state, OrderState::OPEN);
    EXPECT_TRUE(buy->order_id);
    auto open = rig.fake->open_orders();
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(to_str(open[0]->at("side")), "BUY");
    EXPECT_EQ(to_str(open[0]->at("price")), "99900");
    EXPECT_EQ(to_str(open[0]->at("originalSize")), "0.001");

    rig.fake->fill(buy->client_id, "0.0004");
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0004"); }));
    rig.quote();  // wrong side now: cancel the rest of the buy
    EXPECT_FALSE(rig.gw->order());
    EXPECT_TRUE(rig.fake->open_orders().empty());
    rig.quote();
    OrderPtr sell = rig.gw->order();
    ASSERT_TRUE(sell);
    EXPECT_EQ(sell->side, "SELL");
    EXPECT_EQ(sell->price, D("100100.2"));
    EXPECT_EQ(sell->size, D("0.0004"));
    EXPECT_EQ(sell->state, OrderState::OPEN);

    rig.fake->fill(sell->client_id, "0.0004");
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order() && rig.gw->position() == 0; }));
    rig.quote();
    ASSERT_TRUE(rig.gw->order());
    EXPECT_EQ(rig.gw->order()->side, "BUY");
    EXPECT_EQ(rig.gw->order()->size, D("0.001"));
    std::vector<std::string> methods;
    for (auto& [m, body] : rig.fake->posts)
        if (m != "scheduleCancel") methods.push_back(m);
    EXPECT_EQ(methods, (std::vector<std::string> {"placeOrder", "cancelOrder", "placeOrder", "placeOrder"}));
    EXPECT_EQ(rig.gw->stats["rejected"], 0);  // every signature verified
}

TEST(TradingGateway, CancelsOrdersLeftFromEarlierRunsBeforeTrading) {
    GatewayLog log;
    Rig rig({.open_orders = {{"BUY", "90000", "0.01"}}, .closed_orders = {{"SELL", "95000", "0.02"}}});
    EXPECT_TRUE(rig.fake->open_orders().empty());
    std::vector<std::pair<std::string, std::string>> cancels;
    for (auto& [m, body] : rig.fake->posts)
        if (m == "cancelOrder") cancels.emplace_back(to_str(*jget(body, "kind")), to_str(*jget(body, "orderId")));
    // only the open one: recently closed orders in the snapshot are left alone
    EXPECT_EQ(cancels, (std::vector<std::pair<std::string, std::string>> {{"orderId", "0000000000000001"}}));
    EXPECT_FALSE(rig.gw->order());
    EXPECT_EQ(rig.gw->stats["placed"], 0);
    auto lines = log.lines();
    EXPECT_FALSE(matching(lines, R"(>>> CANCEL orderId 0000000000000001; sent on #\d+ main)").empty()) << log.text();
    EXPECT_FALSE(matching(lines, R"(<<< cancel #\d+ main ACCEPTED in [\d.]+ ms: 202 CANCEL_ACKNOWLEDGED, cancel pool 39999 left)").empty());
    EXPECT_TRUE(contains(lines, "<<< order 0000000000000001 not placed by this run: canceled"));
}

TEST(TradingGateway, CancelIsLoggedWhenSentAnsweredAndDone) {
    GatewayLog log;
    Rig rig;
    rig.quote();
    rig.gw->clock_offset_ms = 0.0;  // as measured against GET /v1/time
    log.clear();
    rig.quote(100'100.0);  // target moved beyond tol: cancel
    auto lines = log.lines();
    ASSERT_EQ(lines.size(), 3u) << log.text();
    std::smatch sent, reply;
    std::regex sent_re(R"(>>> CANCEL BUY 0\.001 @ 99900 \[pending cancel\] \(clientId nmm\w+\): target 99990 is [\d.]+ bp away; sent on #(\d+) main)");
    std::regex reply_re(R"(<<< cancel #(\d+) main ACCEPTED in [\d.]+ ms: 202 CANCEL_ACKNOWLEDGED, cancel pool 39999 left)");
    ASSERT_TRUE(std::regex_match(lines[0], sent, sent_re)) << lines[0];
    ASSERT_TRUE(std::regex_match(lines[1], reply, reply_re)) << lines[1];
    EXPECT_EQ(sent[1], reply[1]);
    EXPECT_TRUE(std::regex_match(lines[2], std::regex(R"(<<< CANCELED BUY 0\.001 @ 99900 \[canceled\], [\d.]+ ms after cancel sent \(exchange time [-+][\d.]+ ms, clock offset applied\))")))
        << lines[2];
    EXPECT_GE(rig.gw->delays.count, 1);  // the exchange's stamp on the order event, for the health line
}

TEST(TradingGateway, CancelGoesOutOnEveryLaneAndTheFirstCopyWins) {
    GatewayLog log;
    Rig rig({.cancel_lanes = 2});
    rig.quote();
    log.clear();
    rig.quote(100'100.0);
    std::set<int64_t> stamps;
    int n = 0;
    for (auto& [m, body] : rig.fake->posts)
        if (m == "cancelOrder") {
            ++n;
            stamps.insert(to_int(*jget(body, "timestamp")));
        }
    EXPECT_EQ(n, 3);
    EXPECT_EQ(stamps.size(), 1u);  // one signed request, three copies
    EXPECT_FALSE(rig.gw->order());
    EXPECT_TRUE(rig.fake->open_orders().empty());
    EXPECT_EQ(rig.gw->stats["canceled"], 1);
    EXPECT_EQ(rig.gw->position(), 0);
    auto lines = log.lines();
    EXPECT_FALSE(matching(lines, R"(>>> CANCEL BUY .*; sent on #\d+ main, #1 lane1, #1 lane2)").empty()) << log.text();
    EXPECT_EQ(matching(lines, R"(<<< cancel #\d+ \w+ ACCEPTED in .*)").size(), 1u);
    // Arcus takes a signed timestamp once: the copies that arrive later are refused so
    EXPECT_TRUE(rig.wait([&] { return matching(log.lines(), R"(<<< cancel #\d+ \w+ REFUSED in .*: 401 .*)").size() == 2; }));
    // the losing copies are the expected end of the race, not a warning, and change nothing
    for (auto& r : log.records())
        if (r.name == ORDERS_LOGGER && r.message.find(" 401 ") != std::string::npos) {
            EXPECT_EQ(r.level, logging::INFO) << r.message;
            EXPECT_TRUE(ends_with(r.message, "request timestamp already used (TimestampReused); the same cancel got there first on another connection"))
                << r.message;
        }
    EXPECT_EQ(rig.gw->stats["canceled"], 1);
    EXPECT_EQ(rig.count_posts("cancelOrder"), 3);
    EXPECT_NE(rig.gw->status().find("cancel lanes up 2/2"), std::string::npos);
    EXPECT_NE(rig.gw->status().find("cancel reply median main"), std::string::npos);
}

TEST(TradingGateway, LostMainConnectionPullsTheOrderThroughALane) {
    GatewayLog log;
    Rig rig({.cancel_lanes = 1});
    rig.quote();
    ASSERT_FALSE(rig.fake->open_orders().empty());
    rig.fake->ws->close();  // the main link only; the lane stays up
    ASSERT_TRUE(rig.wait([&] { return rig.fake->open_orders().empty(); }));
    EXPECT_FALSE(matching(log.lines(), R"(>>> CANCEL BUY .*: main connection lost; sent on #1 lane1)").empty()) << log.text();
    ASSERT_TRUE(rig.wait([&] { return rig.gw->ready; }, 10));
    EXPECT_FALSE(rig.gw->order());
    EXPECT_EQ(rig.gw->position(), 0);
}

TEST(TradingGateway, CancelBeforeTheAckThatOvertakesThePlacementIsRepeated) {
    GatewayLog log;
    Rig rig;
    rig.fake->hold_places = true;  // the placement is acknowledged but not booked yet
    rig.strategy->evaluate(view(100'000.0));
    OrderPtr order = rig.gw->order();
    ASSERT_TRUE(order);
    EXPECT_EQ(order->state, OrderState::PENDING_NEW);
    rig.strategy->evaluate(view(100'100.0));  // market moves before the ack: cancel right away
    EXPECT_EQ(order->state, OrderState::PENDING_CANCEL);
    // it finds nothing, goes out once more at once, and finds nothing again
    ASSERT_TRUE(rig.wait([&] { return matching(log.lines(), R"(<<< cancel #\d+ main REFUSED .*ORDER_NOT_FOUND.*)").size() == 2; }))
        << log.text();
    EXPECT_EQ(order->state, OrderState::PENDING_CANCEL);  // not taken for gone: the placement may still land
    rig.fake->release_places();  // ...and it does
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }));
    EXPECT_EQ(order->state, OrderState::CANCELED);
    EXPECT_TRUE(rig.fake->open_orders().empty());
    auto lines = log.lines();
    EXPECT_FALSE(matching(lines, R"(<<< cancel #\d+ main REFUSED .*; it overtook the placement: cancelling again at once.*)").empty());
    EXPECT_FALSE(matching(lines, R"(<<< cancel #\d+ main REFUSED .*; it overtook the placement again: cancelling once .*)").empty());
    EXPECT_FALSE(matching(lines, R"(>>> CANCEL .*: the placement landed after the cancel; sent on #\d+ main)").empty()) << log.text();
}

TEST(TradingGateway, CancelThatOvertookThePlacementGoesAgainWithoutWaitingForTheOrder) {
    GatewayLog log;
    Rig rig;  // one connection: no later copy of the cancel to catch the order
    rig.fake->hold_places = true;
    rig.fake->land_held_on_miss = true;  // the placement lands right behind the cancel that overtook it...
    rig.fake->quiet_landing = true;  // ...and the orders channel never says so
    rig.strategy->evaluate(view(100'000.0));
    OrderPtr order = rig.gw->order();
    rig.strategy->evaluate(view(100'100.0));  // market moves before the ack: cancel right away
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }));
    // without the second cancel the order would rest unseen and be settled as gone after 2 s
    EXPECT_EQ(order->state, OrderState::CANCELED);
    EXPECT_TRUE(rig.fake->open_orders().empty());
    auto lines = log.lines();
    EXPECT_FALSE(matching(lines, R"(>>> CANCEL BUY 0\.001 @ 99900 \[pending cancel\] \(clientId \w+, not acknowledged yet, [\d.]+ ms after placing\): target 99990 is [\d.]+ bp away; sent on #\d+ main)")
                     .empty())
        << log.text();
    EXPECT_FALSE(matching(lines, R"(<<< cancel #\d+ \w+ REFUSED .*; it overtook the placement: cancelling again at once.*)").empty());
    EXPECT_FALSE(matching(lines, R"(>>> CANCEL .*: the cancel overtook the placement; sent on .*)").empty());
    EXPECT_FALSE(matching(lines, R"(<<< CANCELED BUY 0\.001 @ 99900 \[canceled\], [\d.]+ ms after cancel sent.*)").empty());
}

TEST(TradingGateway, FillReportedAfterANotFoundCancelStillCounts) {
    Patch grace(NOT_FOUND_GRACE_S, 0.1);
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    rig.fake->fill(buy->client_id, "0.001", false, false);  // filled; the only event is late
    rig.quote(100'100.0);  // our cancel finds nothing and we settle it after the grace
    EXPECT_FALSE(rig.gw->order());
    EXPECT_EQ(rig.gw->position(), 0);
    rig.fake->push_order(buy->client_id);
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.001"); }));
    rig.quote();
    ASSERT_TRUE(rig.gw->order());
    EXPECT_EQ(rig.gw->order()->side, "SELL");
    EXPECT_EQ(rig.gw->order()->size, D("0.001"));
}

TEST(TradingGateway, RefusedCancelLeavesTheOrderOpen) {
    GatewayLog log;
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->cancel_error = std::pair {429, Json(JsonObject {{"type", "RATE_LIMITED"}, {"message", "too many requests"}})};
    rig.quote(100'100.0);
    EXPECT_EQ(rig.gw->order(), order);
    EXPECT_EQ(order->state, OrderState::OPEN);
    EXPECT_FALSE(order->cancel_sent);
    EXPECT_FALSE(rig.fake->open_orders().empty());
    EXPECT_FALSE(matching(log.lines(), R"(<<< cancel #\d+ main REFUSED in [\d.]+ ms: 429 too many requests \(RATE_LIMITED\); order still open)").empty())
        << log.text();
    rig.fake->cancel_error.reset();
}

TEST(TradingGateway, UnansweredCancelIsLoggedWhenTheConnectionDrops) {
    GatewayLog log;
    Rig rig;
    rig.quote();
    rig.fake->mute_cancels = true;
    rig.strategy->evaluate(view(100'100.0));
    ASSERT_TRUE(rig.wait([&] { return !matching(log.lines(), R"(>>> CANCEL .*)").empty(); }));
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] {
        return !matching(log.lines(), R"(!!! cancel #\d+ main: no reply, connection lost [\d.]+ ms after it was sent)").empty();
    })) << log.text();
    EXPECT_EQ(rig.gw->order()->state, OrderState::PENDING_CANCEL);
    rig.fake->mute_cancels = false;
}

TEST(TradingGateway, ShutdownCancelsWorkingOrderAndDisarmsSwitch) {
    Rig rig;
    rig.quote();
    EXPECT_FALSE(rig.fake->open_orders().empty());
    rig.stop();
    EXPECT_TRUE(rig.fake->open_orders().empty());
    ASSERT_FALSE(rig.fake->dms.empty());
    EXPECT_FALSE(rig.fake->dms.back().has_value());  // disarmed
}

TEST(TradingGateway, NoTradingWhenTheSwitchCannotBeArmed) {
    GatewayLog log;
    Rig rig({.signing_key = unregistered_key(), .wait_ready = false});
    ASSERT_TRUE(rig.wait([&] { return log.text().find("dead man's switch refused") != std::string::npos; }));
    rig.sleep(0.05);
    EXPECT_FALSE(rig.gw->ready);
    EXPECT_FALSE(rig.gw->order());
}

TEST(TradingGateway, BadSignatureRejectsTheOrder) {
    Rig rig({.signing_key = unregistered_key(), .dms_timeout = 0});
    rig.strategy->evaluate(view(100'000.0));
    ASSERT_TRUE(rig.wait([&] { return rig.gw->stats["rejected"] == 1; }));
    EXPECT_FALSE(rig.gw->order());
    EXPECT_TRUE(rig.fake->open_orders().empty());
}

TEST(TradingGateway, ReconnectKeepsTrackingTheWorkingOrder) {
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->ready; }));
    ASSERT_TRUE(rig.wait([&] { return rig.gw->ready; }, 10));
    EXPECT_EQ(rig.gw->order(), order);
    EXPECT_EQ(order->state, OrderState::OPEN);
    rig.fake->fill(order->client_id, "0.001");
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order() && rig.gw->position() == D("0.001"); }));
}

TEST(TradingGateway, APlacementLostWithTheConnectionIsSettledAfterTheReconnect) {
    // ETH run 15: the placement went out as the trading connection dropped and never reached the
    // exchange. After the reconnect it was in no snapshot, its cancels were all answered 202, and
    // no CANCELED ever came: the bot waited forever.
    Patch grace(NOT_FOUND_GRACE_S, 0.1);
    GatewayLog log;
    Rig rig;
    rig.fake->lose_places = rig.fake->ack_unknown_cancels = true;
    rig.strategy->evaluate(view(100'000.0));
    OrderPtr lost = rig.gw->order();
    ASSERT_TRUE(lost);
    EXPECT_EQ(lost->state, OrderState::PENDING_NEW);
    rig.fake->ws->close();  // the trading connection drops
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 10));
    EXPECT_EQ(lost->state, OrderState::CANCELED);
    EXPECT_EQ(lost->reason, "never reached the exchange");
    EXPECT_FALSE(matching(log.lines(), R"(!!! BUY 0\.001 @ 99900 \[pending cancel\] never reached the exchange: settled)").empty()) << log.text();
    rig.fake->lose_places = false;
    ASSERT_TRUE(rig.wait([&] { return rig.gw->ready; }));
    rig.quote();  // quoting goes on
    ASSERT_TRUE(rig.gw->order());
    EXPECT_EQ(rig.gw->order()->state, OrderState::OPEN);
}

TEST(TradingGateway, AnOrderClosedWhileTheConnectionWasDownIsTakenFromTheRecentlyClosedList) {
    Rig rig;
    rig.fake->quiet_orders = rig.fake->ack_unknown_cancels = true;  // its events are lost from here on
    rig.strategy->evaluate(view(100'000.0));
    OrderPtr order = rig.gw->order();
    ASSERT_TRUE(rig.wait([&] { return order->order_id.has_value(); }));  // acknowledged, but no OPEN came
    rig.fake->order_by_client_id(order->client_id)["status"] = "CANCELED";  // e.g. the switch fired
    rig.fake->quiet_orders = false;
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 10));
    EXPECT_EQ(order->state, OrderState::CANCELED);
    EXPECT_FALSE(order->reason);  // the exchange's own CANCELED
}

TEST(TradingGateway, AnAcknowledgedOrderNoSnapshotListHoldsIsSettledAfterTheReconnect) {
    Patch grace(NOT_FOUND_GRACE_S, 0.1);
    Rig rig;
    rig.fake->quiet_orders = rig.fake->ack_unknown_cancels = true;
    rig.strategy->evaluate(view(100'000.0));
    OrderPtr order = rig.gw->order();
    ASSERT_TRUE(rig.wait([&] { return order->order_id.has_value(); }));
    rig.fake->orders.erase(*order->order_id);  // past the recently closed list
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 10));
    EXPECT_EQ(order->reason, "is gone after the reconnect");
}

TEST(TradingGateway, AnUnansweredPlacementThatTheExchangeNeverShowsIsSettledByTheWatchdog) {
    Patch timeout(PENDING_TIMEOUT_S, 0.2);
    Rig rig;
    rig.fake->lose_places = rig.fake->ack_unknown_cancels = true;
    rig.strategy->evaluate(view(100'000.0));
    OrderPtr lost = rig.gw->order();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 5));  // no answer -> cancel -> still nothing -> settled
    EXPECT_EQ(lost->reason, "never reached the exchange");
    EXPECT_EQ(lost->cancels, 1);
}

TEST(TradingGateway, ACancelledOrderWhoseCanceledEventIsLostIsSettledByTheOpenOrders) {
    // ETH run 20: cancelled over a lane while the main connection was down, the snapshot after the
    // reconnect still OPEN, then 15 minutes of re-cancels that each got 202 and no CANCELED.
    Patch timeout(PENDING_TIMEOUT_S, 0.2);
    GatewayLog log;
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    EXPECT_EQ(order->state, OrderState::OPEN);
    rig.fake->quiet_orders = rig.fake->ack_unknown_cancels = true;  // the cancel acts, its event is lost
    rig.strategy->evaluate(view(100'100.0));  // the target moved: cancel
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 5));
    EXPECT_EQ(order->state, OrderState::CANCELED);
    EXPECT_EQ(order->reason, "not open on the exchange");
    EXPECT_EQ(rig.fake->open_orders_requests, 1);
    EXPECT_FALSE(matching(log.lines(), R"(!!! .* is not among the exchange.s open orders .*; settled)").empty()) << log.text();
}

TEST(TradingGateway, ACancelThatHearsNothingIsSentAgainWhileTheOrderIsStillOpen) {
    Patch timeout(PENDING_TIMEOUT_S, 0.2);
    GatewayLog log;
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->mute_cancels = true;  // the cancel never acts: the order rests
    rig.strategy->evaluate(view(100'100.0));
    ASSERT_TRUE(rig.wait([&] { return !matching(log.lines(), R"(!!! no answer for .*; cancelling by client id)").empty(); }, 5));
    EXPECT_EQ(rig.gw->order(), order);
    EXPECT_EQ(order->state, OrderState::PENDING_CANCEL);  // open on the exchange: not settled
    EXPECT_GE(rig.fake->open_orders_requests, 1);
    rig.fake->open_orders_error = 503;  // and when the exchange cannot tell, nothing is settled either
    int before = order->cancels;
    ASSERT_TRUE(rig.wait([&] { return order->cancels > before; }, 5));
    EXPECT_EQ(rig.gw->order(), order);
    rig.fake->mute_cancels = false;
}

TEST(TradingGateway, CancelOfAnOrderAlreadyGoneSettlesIt) {
    Patch grace(NOT_FOUND_GRACE_S, 0.1);
    GatewayLog log;
    Rig rig;
    rig.quote();
    (*rig.fake->open_orders()[0])["status"] = "CANCELED";  // gone without us hearing about it
    rig.quote(100'100.0);  // target moved: cancel -> not found -> settled after the grace
    EXPECT_FALSE(rig.gw->order());
    EXPECT_EQ(rig.gw->stats["canceled"], 1);
    auto lines = log.lines();
    EXPECT_FALSE(matching(lines, R"(<<< cancel #\d+ main REFUSED in [\d.]+ ms: 404 order not found \(ORDER_NOT_FOUND\))").empty()) << log.text();
    EXPECT_FALSE(matching(lines, R"(<<< CANCELED BUY 0\.001 @ 99900 \[canceled\]: not found, [\d.]+ ms after cancel sent)").empty());
}

namespace {

Json trades(std::vector<std::pair<std::string, std::string>> rows, std::optional<int64_t> timestamp = std::nullopt) {
    JsonArray contents;
    for (auto& [p, s] : rows) {
        JsonObject t {{"price", p}, {"size", s}};
        if (timestamp) t["timestamp"] = *timestamp;
        contents.push_back(t);
    }
    return JsonObject {{"type", "channel_data"}, {"channel", "trades"}, {"id", "BTC-USD"}, {"contents", contents}};
}

}  // namespace

TEST(PaperGateway, OrdersFillAgainstPublicTrades) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    PaperGateway gw(btc_market(), "ws://unused");
    gw.ready = true;
    OrderPtr buy = gw.place("BUY", D("99900"), D("0.001"));
    EXPECT_EQ(buy->state, OrderState::OPEN);
    gw.on_message(trades({{"99950", "1"}}));  // above our bid: not us
    EXPECT_EQ(buy->filled, 0);
    gw.on_message(trades({{"99900", "0.0003"}, {"99800", "5"}}));
    EXPECT_EQ(buy->state, OrderState::FILLED);
    EXPECT_EQ(gw.position(), D("0.001"));
    EXPECT_FALSE(gw.order());
    OrderPtr sell = gw.place("SELL", D("100100.2"), D("0.001"));
    gw.on_message(trades({{"100100.1", "1"}, {"100100.2", "0.0004"}}));
    EXPECT_EQ(sell->filled, D("0.0004"));
    EXPECT_EQ(gw.position(), D("0.0006"));
    gw.cancel(sell);
    EXPECT_EQ(sell->state, OrderState::PENDING_CANCEL);  // it takes a moment to act, as live
    run_for(ioc, 0.05);
    EXPECT_EQ(sell->state, OrderState::CANCELED);
    EXPECT_FALSE(gw.order());
}

TEST(PaperGateway, ACancelDoesNotEscapeTheSweepItAnswers) {
    // The book frame showing a sweep arrives before its trades frame, the bot pulls the order on
    // it, and an instant paper cancel let the order escape the very fill that costs a live one.
    // Trades from before the order was placed fill nothing.
    boost::asio::io_context ioc;
    set_loop(ioc);
    PaperGateway gw(btc_market(), "ws://unused");
    gw.ready = true;
    int64_t old = time_ns() / 1000 - 1'000'000;
    OrderPtr buy = gw.place("BUY", D(99900), D("0.001"));
    gw.on_message(trades({{"99800", "1"}}, old));
    EXPECT_EQ(buy->filled, 0);  // printed a second before the order existed
    gw.cancel(buy, "the book came to it");  // the book frame of the sweep
    gw.on_message(trades({{"99890", "1"}}, time_ns() / 1000));
    EXPECT_EQ(buy->state, OrderState::FILLED);  // its trades frame, a moment later
    EXPECT_EQ(gw.position(), D("0.001"));
}

TEST(PaperGateway, FillsEachSideAgainstTheTradesThatCrossIt) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    PaperGateway gw(btc_market(), "ws://unused");
    gw.ready = true;
    OrderPtr buy = gw.place("BUY", D(99900), D("0.001"));
    OrderPtr sell = gw.place("SELL", D(100100), D("0.001"));
    EXPECT_EQ(gw.working(), (std::vector<OrderPtr> {buy, sell}));
    gw.on_message(trades({{"100000", "1"}}));  // between them: neither
    EXPECT_EQ(buy->filled, 0);
    EXPECT_EQ(sell->filled, 0);
    gw.on_message(trades({{"100150", "0.0004"}, {"99850", "0.002"}}));
    EXPECT_EQ(sell->filled, D("0.0004"));
    EXPECT_EQ(buy->state, OrderState::FILLED);
    EXPECT_EQ(gw.position(), D("0.0006"));
    EXPECT_EQ(gw.working(), (std::vector<OrderPtr> {sell}));
    gw.cancel(sell);
    run_for(ioc, 0.05);
    EXPECT_TRUE(gw.working().empty());
    EXPECT_EQ(sell->state, OrderState::CANCELED);
}

TEST(TradingGateway, CancelGoesOutSignedAhead) {
    Rig rig({.presign_cancel = 5.0});
    rig.quote();
    ASSERT_TRUE(rig.gw->order()->presigned);  // signed right after placing
    int64_t ts = std::get<0>(*rig.gw->order()->presigned);
    rig.quote(100'100.0);
    Json cancel;
    for (auto& [m, body] : rig.fake->posts)
        if (m == "cancelOrder") cancel = body;
    EXPECT_EQ(to_int(*jget(cancel, "timestamp")), ts);  // the request signed ahead, verified by the fake
    EXPECT_FALSE(rig.gw->order());
    EXPECT_EQ(rig.gw->stats["canceled"], 1);
}

TEST(TradingGateway, SignatureMadeAheadIsKeptFresh) {
    Rig rig({.presign_cancel = 0.2});
    rig.quote();
    int64_t first = std::get<0>(*rig.gw->order()->presigned);
    rig.sleep(0.35);
    EXPECT_GT(std::get<0>(*rig.gw->order()->presigned), first);
}

TEST(TradingGateway, RefusedCancelSignedAheadIsResentSignedNow) {
    GatewayLog log;
    Rig rig({.presign_cancel = 5.0});
    rig.quote();
    rig.fake->max_cancel_age_ms = 50;  // this exchange wants fresh timestamps
    rig.sleep(0.1);
    rig.strategy->evaluate(view(100'100.0));
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }));
    EXPECT_EQ(rig.gw->stats["canceled"], 1);
    EXPECT_TRUE(rig.fake->open_orders().empty());
    EXPECT_EQ(rig.gw->presign_cancel, 0);  // signed when sent from now on
    auto lines = log.lines();
    EXPECT_FALSE(matching(lines, R"(<<< cancel #\d+ main REFUSED in [\d.]+ ms: 400 timestamp too old \(INVALID_TIMESTAMP\); re-sending, signed now.*)").empty())
        << log.text();
    EXPECT_FALSE(matching(lines, R"(>>> CANCEL .*: re-sent, signed now; sent on #\d+ main)").empty());
}

TEST(TradingGateway, OrderEventsHaveTheirOwnLogger) {
    GatewayLog log;
    Rig rig;
    rig.quote();
    rig.fake->fill(rig.gw->order()->client_id, "0.0004");
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0004"); }));
    rig.quote();  // wrong side now: cancel the rest
    std::vector<std::string> events, others;
    for (auto& r : log.records()) (r.name == ORDERS_LOGGER ? events : others).push_back(r.message);
    for (const char* pattern : {R"(>>> PLACE BUY .*)", R"(<<< FILL BUY 0\.0004 .*-> position 0\.0004.*)", R"(>>> CANCEL .*)",
                                R"(<<< cancel #\d+ main ACCEPTED in .*)", R"(<<< CANCELED BUY .*)"})
        EXPECT_FALSE(matching(events, pattern).empty()) << pattern;
    EXPECT_TRUE(matching(others, R"((>>>|<<<|!!!) .*)").empty());  // connection and readiness only
}

TEST(TradingGateway, TwinRefusedForItsTimestampKeepsCancelsSignedAhead) {
    GatewayLog log;
    Rig rig({.cancel_lanes = 1, .presign_cancel = 5.0});
    rig.fake->cancel_event_delay = 0.05;  // like Arcus: the loser's answer comes before the order is reported gone
    rig.quote();
    rig.quote(100'100.0);  // cancelled with the signature made ahead, on both connections
    EXPECT_FALSE(rig.gw->order());
    rig.sleep(0.05);  // room for anything re-sent and its answers
    EXPECT_EQ(rig.gw->presign_cancel, 5.0);  // the loser of the race is no sign of a stale signature
    EXPECT_TRUE(matching(log.lines(), R"(.*re-sent, signed now.*)").empty());
    EXPECT_EQ(rig.count_posts("cancelOrder"), 2);  // one request, two copies, nothing re-sent
    EXPECT_EQ(matching(log.lines(), R"(<<< cancel .* REFUSED .*TimestampReused.*)").size(), 1u) << log.text();
}

TEST(TradingGateway, TwinRefusedForItsTimestampHoldsNothingUp) {
    GatewayLog log;
    Rig rig({.cancel_lanes = 1, .presign_cancel = 5.0});
    rig.fake->cancel_event_delay = 0.05;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.strategy->evaluate(view(100'100.0));  // cancelled on both connections
    ASSERT_TRUE(rig.wait([&] { return !matching(log.lines(), R"(<<< cancel .* REFUSED .*TimestampReused.*)").empty(); }));
    EXPECT_TRUE(order->state == OrderState::PENDING_CANCEL || order->state == OrderState::CANCELED);  // still on its way out
    EXPECT_FALSE(rig.gw->cooling_down());
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }));
    rig.strategy->evaluate(view(100'100.0));
    ASSERT_TRUE(rig.gw->order());  // the replacement leaves at once
    EXPECT_EQ(rig.gw->order()->side, "BUY");
}

TEST(TradingGateway, PublicTradeShowsOurFillBeforeTheOrdersChannel) {
    GatewayLog log;
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    rig.fake->fill(buy->client_id, "0.001", false);  // the public trade only, for now
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.001"); }));
    EXPECT_EQ(buy->state, OrderState::FILLED);
    EXPECT_EQ(buy->reason, "seen in public trades");
    EXPECT_FALSE(rig.gw->order());
    rig.quote();
    EXPECT_EQ(rig.gw->order()->side, "SELL");  // already selling, before the orders channel said a word
    rig.fake->push_order(buy->client_id);  // now it does
    ASSERT_TRUE(rig.wait([&] { return !matching(log.lines(), R"(<<< orders channel: BUY 0\.001 @ 99900 filled 0\.001, .*)").empty(); }))
        << log.text();
    EXPECT_EQ(rig.gw->position(), D("0.001"));
    EXPECT_EQ(rig.gw->stats["fills"], 1);  // counted once
    EXPECT_FALSE(matching(log.lines(), R"(<<< FILL BUY 0\.001 @ 99900 \(order 0\.001/0\.001, id \w+\) -> position 0\.001, seen in public trades)").empty());
}

TEST(TradingGateway, FillSeenOnBothChannelsCountsOnceWhicheverComesFirst) {
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    rig.fake->fill(buy->client_id, "0.0004", true, false);  // the orders channel first
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0004"); }));
    JsonObject order = rig.fake->order_by_client_id(buy->client_id);
    rig.fake->publish_trade(order, "0.0004", std::nullopt, "t1");  // then the same fill in public
    rig.fake->publish_trade(order, "0.0004", std::nullopt, "t1");  // and the same trade again
    rig.fake->publish_trade(order, "0.0003", "0000000000000099");  // someone else's order
    rig.sleep(0.05);
    EXPECT_EQ(rig.gw->position(), D("0.0004"));
    EXPECT_EQ(buy->filled, D("0.0004"));
    rig.fake->publish_trade(order, "0.0002", std::nullopt, "t2");  // a new fill: in public first
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0006"); }));
    rig.fake->fill(buy->client_id, "0.0002", true, false);  // the orders channel catches up: 0.0006 in all
    rig.sleep(0.05);
    EXPECT_EQ(rig.gw->position(), D("0.0006"));
    EXPECT_EQ(buy->filled, D("0.0006"));
}

TEST(TradingGateway, OrderTheExchangeHasNotNamedYetIsFoundByAddressSideAndPrice) {
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    JsonObject order = rig.fake->order_by_client_id(buy->client_id);
    buy->order_id.reset();  // as if neither the reply nor the orders channel had named it yet
    rig.fake->publish_trade(order, "0.001");
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.001"); }));
    EXPECT_EQ(buy->state, OrderState::FILLED);
}

TEST(TradingGateway, UserFillsCountTheFillFirstAndTheLaterSourcesAreTimedAgainstIt) {
    GatewayLog log;
    Rig rig;
    EXPECT_FALSE(matching(log.lines(), "<<< subscribed to trades").empty());
    EXPECT_FALSE(matching(log.lines(), "<<< subscribed to userFills").empty());
    rig.quote();
    OrderPtr buy = rig.gw->order();
    // as live: userFills first, then the orders channel; the public trade (same tradeId) comes last
    rig.fake->fill(buy->client_id, "0.001", true, false, true, "777");
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.001"); }));
    rig.fake->publish_trade(rig.fake->order_by_client_id(buy->client_id), "0.001", std::nullopt, "777");
    ASSERT_TRUE(rig.wait([&] {
        return !matching(log.lines(), R"(<<< public trade: BUY 0\.001 @ 99900 \(id \w+, trade 777\) filled, [\d.]+ ms after the userFills, [\d.]+ ms after the orders channel)")
                    .empty();
    })) << log.text();
    auto lines = log.lines();
    EXPECT_FALSE(matching(lines, R"(<<< FILL BUY 0\.001 @ 99900 \(order 0\.001/0\.001, id \w+\) -> position 0\.001, seen on userFills)").empty());
    EXPECT_FALSE(matching(lines, R"(<<< FILLED BUY 0\.001 @ 99900 \[filled, filled 0\.001\]: seen on userFills)").empty());
    // one line per fill out of {"isSnapshot": false, "fills": [...]}, after the fill is counted
    EXPECT_FALSE(matching(lines, R"(<<< userFills BUY 0\.001 @ 99900 MAKER OPEN_LONG \(trade 777, order \w+, pnl 0, fee 0, left [\d.]+\), -?[\d.]+ ms after the match \(host clock\); first)")
                     .empty())
        << log.text();
    EXPECT_FALSE(matching(lines, R"(<<< orders channel: BUY 0\.001 @ 99900 filled 0\.001, [\d.]+ ms after the userFills)").empty());
    EXPECT_EQ(rig.gw->position(), D("0.001"));
    EXPECT_EQ(buy->filled, D("0.001"));  // counted once
    EXPECT_EQ(rig.gw->stats["fills"], 1);
}

TEST(TradingGateway, AFillOnlyUserFillsShowsCountsAndTheOrdersChannelAddsNothing) {
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    rig.fake->fill(buy->client_id, "0.0004", false, false, true);
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0004"); }));
    EXPECT_EQ(buy->filled, D("0.0004"));
    EXPECT_EQ(buy->state, OrderState::OPEN);  // a partial fill: still working
    rig.fake->push_order(buy->client_id);  // the orders channel catches up
    rig.sleep(0.05);
    EXPECT_EQ(rig.gw->position(), D("0.0004"));
    EXPECT_EQ(rig.gw->stats["fills"], 1);
    rig.fake->fill(buy->client_id, "0.0006", false, false, true);  // the rest
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }));
    EXPECT_EQ(buy->state, OrderState::FILLED);
    EXPECT_EQ(rig.gw->position(), D("0.001"));
}

TEST(TradingGateway, PartialFillsSeenOnDifferentTradeSourcesAddUp) {
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    rig.fake->fill(buy->client_id, "0.0004", false, true, false, "a");  // public trades only
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0004"); }));
    // a second fill on userFills only, in a frame without remainingSize: only the trades add up to 0.0007
    JsonObject f {{"tradeId", "b"}, {"orderId", *buy->order_id}, {"marketId", 1}, {"clientId", buy->client_id}, {"side", "BUY"},
                  {"size", "0.0003"}, {"price", "99900"}, {"role", "MAKER"}, {"sequenceNumber", rig.fake->seq + 1}};
    rig.fake->broadcast(rig.fake->frame("channel_data", "userFills", JsonObject {{"isSnapshot", false}, {"fills", JsonArray {f}}}));
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0007"); }));
    rig.fake->publish_trade(rig.fake->order_by_client_id(buy->client_id), "0.0003", std::nullopt, "b");  // same trade
    JsonObject first = f;
    first["tradeId"] = "a";
    first["size"] = "0.0004";
    rig.fake->broadcast(rig.fake->frame("channel_data", "userFills", JsonObject {{"isSnapshot", false}, {"fills", JsonArray {first}}}));  // late
    rig.sleep(0.05);
    EXPECT_EQ(rig.gw->position(), D("0.0007"));
    EXPECT_EQ(buy->filled, D("0.0007"));
    EXPECT_EQ(rig.gw->stats["fills"], 2);
}

TEST(TradingGateway, UserFillsOfASnapshotOrOfSomeoneElsesOrderDoNotCount) {
    GatewayLog log;
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    JsonObject f {{"tradeId", "s1"}, {"orderId", *buy->order_id}, {"marketId", 1}, {"clientId", buy->client_id}, {"side", "BUY"},
                  {"size", "0.0005"}, {"price", "99900"}, {"remainingSize", "0.0005"}, {"sequenceNumber", rig.fake->seq + 1}};
    rig.fake->broadcast(rig.fake->frame("channel_data", "userFills", JsonObject {{"isSnapshot", true}, {"fills", JsonArray {f}}}));
    JsonObject other = f;
    other["tradeId"] = "o1";
    other["orderId"] = "00000000000000ff";
    other["clientId"] = "someone-else";
    rig.fake->broadcast(rig.fake->frame("channel_data", "userFills", JsonObject {{"isSnapshot", false}, {"fills", JsonArray {other}}}));
    ASSERT_TRUE(rig.wait([&] { return !matching(log.lines(), R"(<<< userFills BUY .*\(trade o1, .*; not one of this run's orders)").empty(); }))
        << log.text();
    EXPECT_TRUE(contains(log.lines(), "<<< userFills snapshot of 1 fills: not counted, the positions snapshot has them"));
    EXPECT_EQ(rig.gw->position(), 0);
    EXPECT_EQ(buy->filled, 0);
}

TEST(TradingGateway, AUserFillNumberedBeforeTheSnapshotStillCountsForAnOrderPlacedAfterIt) {
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    JsonObject f {{"tradeId", "q1"}, {"orderId", *buy->order_id}, {"marketId", 1}, {"clientId", buy->client_id}, {"side", "BUY"},
                  {"size", "0.0002"}, {"price", "99900"}, {"remainingSize", "0.0008"}, {"sequenceNumber", 0}};
    rig.fake->broadcast(rig.fake->frame("channel_data", "userFills", JsonObject {{"isSnapshot", false}, {"fills", JsonArray {f}}}));
    EXPECT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0002"); }));  // not taken for a fill the snapshot already had
}

TEST(TradingGateway, TheTakerOrderThatHitUsIsLoggedOnceTaggedAtTouchOrSweep) {
    GatewayLog log;
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    JsonObject order = rig.fake->order_by_client_id(buy->client_id);
    rig.fake->publish_trade(order, "0.0002");  // a taker that took only our price
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0002"); }));
    EXPECT_FALSE(matching(log.lines(), R"(<<< taker taker SELL: 1 trades, 1 prices 99900\.\.99900, 0\.0002 in all; ours 0\.0002, price 1 of 1, 0 ticks from its first: at-touch)")
                     .empty())
        << log.text();
    // another one cleared two better levels first and went one past ours
    rig.fake->publish_sweep(order, "0.0003", {"99910", "99905"}, {"99895"});
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0005"); }));
    rig.fake->publish_sweep(order, "0.0001", {"99905"}, {}, "third");  // a better price, then ours
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0006"); }));
    std::vector<std::string> third, sweeper;
    for (auto& line : log.lines()) {
        if (starts_with(line, "<<< taker third ")) third.push_back(line);
        if (starts_with(line, "<<< taker sweeper ")) sweeper.push_back(line);
    }
    EXPECT_EQ(third, (std::vector<std::string> {"<<< taker third SELL: 2 trades, 2 prices 99905..99900, 0.0021 in all; ours 0.0001, price 2 of 2, "
                                                "50 ticks from its first: sweep down to us"}));
    EXPECT_EQ(sweeper, (std::vector<std::string> {"<<< taker sweeper SELL: 4 trades, 4 prices 99910..99895, 0.0063 in all; ours 0.0003, price 3 of 4, "
                                                  "100 ticks from its first: sweep through us"}));
}

TEST(TradingGateway, ServerErrorsReachTheOrdersLog) {
    GatewayLog log;
    Rig rig;
    for (auto& s : rig.fake->subscribers) s->send(R"({"type": "error", "message": "unknown channel"})");
    EXPECT_TRUE(rig.wait([&] { return !matching(log.lines(), R"(!!! server error: \{"type":"error","message":"unknown channel"\})").empty(); }))
        << log.text();
}

TEST(TradingGateway, ASwitchReArmWithoutAnAnswerIsLogged) {
    GatewayLog log;
    Rig rig({.dms_timeout = 0.2});
    rig.fake->mute_dms = true;
    EXPECT_TRUE(rig.wait([&] { return !matching(log.lines(), R"(!!! dead man's switch: the re-arm sent \d+ s ago got no answer)").empty(); }))
        << log.text();
}

TEST(TradingGateway, ALostPlacementWhoseCancelWasRefusedIsCancelledAgainAndSettled) {
    // The refused cancel put the unseen order back to PENDING_NEW, and the watchdog neither settled
    // nor re-cancelled it.
    Patch timeout(PENDING_TIMEOUT_S, 0.2);
    Rig rig;
    rig.fake->lose_places = true;
    rig.fake->cancel_error = std::pair {429, Json(JsonObject {{"type", "RATE_LIMITED"}, {"message", "slow down"}})};
    rig.strategy->evaluate(view(100'000.0));
    OrderPtr lost = rig.gw->order();
    rig.gw->cancel(lost, "test");
    ASSERT_TRUE(rig.wait([&] { return lost->state == OrderState::PENDING_NEW && lost->cancels == 1; }));  // refused: back
    rig.fake->cancel_error.reset();
    rig.fake->ack_unknown_cancels = true;
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 5));  // re-cancelled by the watchdog, then settled
    EXPECT_EQ(lost->reason, "never reached the exchange");
    EXPECT_GE(lost->cancels, 2);
}

TEST(TradingGateway, AnOrderThatLandsAfterACancelAnswered202IsCancelledAgain) {
    // Arcus answers 202 to a cancel of an order it does not have yet; an order landing after such a
    // cancel rested until the 10 s watchdog.
    Rig rig;
    rig.fake->hold_places = rig.fake->ack_unknown_cancels = true;
    rig.strategy->evaluate(view(100'000.0));
    OrderPtr order = rig.gw->order();
    ASSERT_TRUE(rig.wait([&] { return rig.gw->order() == order && order->state == OrderState::PENDING_NEW; }));
    rig.gw->cancel(order, "test");  // overtakes the placement: 202, nothing done
    ASSERT_TRUE(rig.wait([&] { return order->cancel_accepted; }));
    rig.fake->release_places();  // the placement lands and rests
    ASSERT_TRUE(rig.wait([&] { return order->state == OrderState::CANCELED; }, 2));  // cancelled again at once
    EXPECT_TRUE(rig.fake->open_orders().empty());
}

TEST(TradingGateway, ACancelReadFromTheReconnectSnapshotOpensNoStaleLevelWindow) {
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->quiet_orders = true;  // its CANCELED is lost with the connection
    rig.fake->order_by_client_id(order->client_id)["status"] = "CANCELED";
    rig.fake->quiet_orders = false;
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 10));
    EXPECT_EQ(order->state, OrderState::CANCELED);
    EXPECT_FALSE(rig.gw->recently_cancelled("BUY"));
}

TEST(TradingGateway, AnOrderAStaleSnapshotListsAsOpenIsLookedUpAndForgotten) {
    // A reconnect snapshot listed as OPEN an order the exchange no longer had. Its cancel got 202 and
    // nothing else, so it stayed a leftover being cancelled, and the gateway never became ready again.
    Patch stray(STRAY_CHECK_S, 0.2);
    GatewayLog log;
    Rig rig;
    rig.fake->ack_unknown_cancels = true;
    rig.fake->stale_open = {JsonObject {{"orderId", "00000000000000ff"}, {"clientId", "gone"}, {"marketId", 1}, {"side", "BUY"},
                                        {"status", "OPEN"}, {"price", "90000"}, {"originalSize", "0.01"}, {"filledSize", "0"},
                                        {"remainingSize", "0.01"}}};
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->ready; }));
    ASSERT_TRUE(rig.wait([&] { return rig.gw->ready; }, 10));
    auto lines = log.lines();
    EXPECT_TRUE(contains(lines, "!!! cancelling order not placed by this run: BUY 0.01 @ 90000")) << log.text();
    EXPECT_TRUE(contains(lines, "!!! order 00000000000000ff not placed by this run is not among the exchange's open orders: forgotten"));
    rig.quote();
    ASSERT_TRUE(rig.gw->order());
    EXPECT_EQ(rig.gw->order()->state, OrderState::OPEN);  // quoting again
}

TEST(TradingGateway, OurOrderAStaleSnapshotListsAsOpenIsLookedUpSoonNotAfterTheTimeout) {
    // 19 Oct: cancelled over a lane while the main connection was down; the snapshot after the
    // reconnect still listed it OPEN, its re-cancel got 202 and nothing else, and the side stayed held
    // until GET /v1/openOrders was asked after PENDING_TIMEOUT_S. It is asked SNAPSHOT_RECHECK_S after
    // the re-cancel now.
    Patch recheck(SNAPSHOT_RECHECK_S, 0.1);
    GatewayLog log;
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->quiet_orders = rig.fake->ack_unknown_cancels = true;  // the cancel acts, its CANCELED is lost
    rig.strategy->evaluate(view(100'100.0));  // the target moved: cancel
    ASSERT_TRUE(rig.wait([&] { return to_str(rig.fake->order_by_client_id(order->client_id).at("status")) == "CANCELED"; }));
    EXPECT_EQ(order->state, OrderState::PENDING_CANCEL);
    JsonObject gone = rig.fake->order_by_client_id(order->client_id);
    rig.fake->snapshot_hide = {order->client_id};  // the snapshot shows it OPEN still
    gone["status"] = "OPEN";
    gone["state"] = "OPEN";
    rig.fake->stale_open = {gone};
    rig.fake->quiet_orders = false;
    double started = monotonic();
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return order->state == OrderState::CANCELED; }, 5));
    EXPECT_LT(monotonic() - started, PENDING_TIMEOUT_S / 2);
    EXPECT_EQ(order->reason, "not open on the exchange");
    auto lines = log.lines();
    EXPECT_FALSE(matching(lines, R"(>>> CANCEL BUY 0\.001 @ 99900 .*still resting after reconnect.*)").empty()) << log.text();
    EXPECT_FALSE(matching(lines, R"(!!! .* is not among the exchange.s open orders .* the snapshot after the reconnect listed it open and its cancel heard nothing since; settled)")
                     .empty());
}

TEST(TradingGateway, ALeftoverOrderWhoseCancelWasRefusedIsCancelledAgain) {
    Patch stray(STRAY_CHECK_S, 0.2);
    GatewayLog log;
    Rig rig;
    rig.fake->new_order("an-earlier-run", "BUY", "90000", "0.01");  // really resting
    rig.fake->cancel_error = std::pair {429, Json(JsonObject {{"message", "rate limited"}})};
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !matching(log.lines(), R"(<<< cancel #\d+ main REFUSED.*rate limited.*)").empty(); }, 10)) << log.text();
    rig.fake->cancel_error.reset();
    ASSERT_TRUE(rig.wait([&] { return rig.gw->ready; }, 10));
    EXPECT_TRUE(rig.fake->open_orders().empty());
    EXPECT_TRUE(contains(log.lines(), "!!! order 0000000000000001 not placed by this run is still open: cancelling it again")) << log.text();
}

TEST(TradingGateway, ASnapshotBehindOnFillsDoesNotCountAFillTwice) {
    // The reconnect snapshot showed the working order less filled than we had seen; taking that
    // lowered the order's fill, and its next event counted the difference again.
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->fill(order->client_id, "0.0004");
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0004"); }));
    rig.fake->snapshot_filled[order->client_id] = "0";
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->ready; }));
    ASSERT_TRUE(rig.wait([&] { return rig.gw->ready; }, 10));
    EXPECT_EQ(order->filled, D("0.0004"));
    rig.fake->fill(order->client_id, "0.0001");
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("0.0005"); }));
    rig.sleep(0.1);
    EXPECT_EQ(rig.gw->position(), D("0.0005"));
}

TEST(TradingGateway, ARestingOrderLeftOutOfTheReconnectSnapshotIsCancelled) {
    // An order that had rested and was in no list of a lagging snapshot was settled as cancelled
    // with no cancel sent: it could rest on next to a new one.
    Patch grace(NOT_FOUND_GRACE_S, 0.1);
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->snapshot_hide.insert(order->client_id);  // it still rests
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 10));
    EXPECT_TRUE(rig.fake->open_orders().empty());
    EXPECT_EQ(order->state, OrderState::CANCELED);
}

TEST(TradingGateway, ARestingOrderGoneWhileDisconnectedIsSettledByTheOpenOrders) {
    Patch grace(NOT_FOUND_GRACE_S, 0.1);
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->ack_unknown_cancels = true;
    rig.fake->orders.erase(*order->order_id);  // gone, and past the recently closed list
    rig.fake->ws->close();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 10));
    EXPECT_EQ(order->reason, "not open on the exchange");
}

TEST(TradingGateway, TheWatchdogSurvivesAnOpenOrdersAnswerWithoutAList) {
    // {"orders": null} raised outside the try and ended the watchdog for good.
    Patch timeout(PENDING_TIMEOUT_S, 0.2);
    Rig rig;
    rig.quote();
    OrderPtr order = rig.gw->order();
    rig.fake->quiet_orders = rig.fake->ack_unknown_cancels = true;
    rig.fake->open_orders_body = Json(JsonObject {{"orders", nullptr}});
    rig.strategy->evaluate(view(100'100.0));
    ASSERT_TRUE(rig.wait([&] { return rig.fake->open_orders_requests >= 2; }, 5));  // still at it
    rig.fake->open_orders_body.reset();
    ASSERT_TRUE(rig.wait([&] { return !rig.gw->order(); }, 5));
    EXPECT_EQ(order->reason, "not open on the exchange");
}

TEST(TradingGateway, ShutdownKeepsTheSwitchArmedWhenTheCancelDoesNotGoThrough) {
    // The disarm went out with the cancel; a refused cancel left the order resting with no switch to cancel it.
    auto log = std::make_unique<GatewayLog>();
    Rig rig;
    rig.quote();
    rig.fake->cancel_error = std::pair {503, Json(JsonObject {{"message", "unavailable"}})};
    rig.stop();
    EXPECT_FALSE(rig.fake->open_orders().empty());
    for (auto& d : rig.fake->dms) EXPECT_TRUE(d.has_value());  // never disarmed
    EXPECT_NE(log->text().find("the dead man's switch stays armed"), std::string::npos) << log->text();
}

TEST(TradingGateway, ShutdownCancelsAgainAfterARefusedCancelThenDisarms) {
    Rig rig;
    rig.quote();
    rig.fake->cancel_error = std::pair {503, Json(JsonObject {{"message", "unavailable"}})};
    auto recover = call_later(0.3, [&] { rig.fake->cancel_error.reset(); });
    rig.stop();
    EXPECT_TRUE(rig.fake->open_orders().empty());
    ASSERT_FALSE(rig.fake->dms.empty());
    EXPECT_FALSE(rig.fake->dms.back().has_value());
}

TEST(TradingGateway, PlacementsRejectedInARowBackOff) {
    // A placement the exchange kept rejecting was sent again every 0.5 s forever, draining the order
    // pool every bot on the subaccount shares.
    Patch cooldown(REJECT_COOLDOWN_S, 0.05);
    GatewayLog log;
    Rig rig({.signing_key = unregistered_key(), .dms_timeout = 0});
    double end = monotonic() + 1.0;
    while (monotonic() < end) {
        if (!rig.gw->order() && !rig.gw->busy()) rig.strategy->evaluate(view(100'000.0));
        rig.sleep(0.005);
    }
    // 0.05 s apart 3 times, then 0.1, 0.2, 0.4 ...: 7 or so in the second, not 20
    EXPECT_GE(rig.gw->stats["rejected"], 5);
    EXPECT_LE(rig.gw->stats["rejected"], 8);
    EXPECT_NE(log.text().find("placements rejected in a row: not placing for"), std::string::npos);
}

TEST(TradingGateway, LiveBothOrdersRestFillApartAndBothGoAtShutdown) {
    Rig rig;
    rig.strategy = std::make_unique<QuoteStrategy>(
        rig.gw.get(), btc_market(), StrategyOptions {.markup = 0.001, .tol = 0.0002, .order_size = 0.001, .direction = "both"});
    rig.quote();
    OrderPtr buy = rig.gw->orders.at("BUY"), sell = rig.gw->orders.at("SELL");
    ASSERT_TRUE(rig.wait([&] { return buy->state == OrderState::OPEN && sell->state == OrderState::OPEN; }));
    std::vector<std::pair<std::string, std::string>> open;
    for (auto* o : rig.fake->open_orders()) open.emplace_back(to_str(o->at("side")), to_str(o->at("price")));
    std::sort(open.begin(), open.end());
    EXPECT_EQ(open, (std::vector<std::pair<std::string, std::string>> {{"BUY", "99900"}, {"SELL", "100100.2"}}));
    rig.fake->fill(sell->client_id, "0.0004");
    ASSERT_TRUE(rig.wait([&] { return rig.gw->position() == D("-0.0004"); }));
    EXPECT_EQ(sell->filled, D("0.0004"));
    EXPECT_EQ(buy->filled, 0);  // the fill went to its own order
    EXPECT_NE(rig.gw->status().find("BUY 0.001 @ 99900 [open], SELL 0.001 @ 100100.2 [open, filled 0.0004]"), std::string::npos)
        << rig.gw->status();
    rig.stop();
    EXPECT_TRUE(rig.fake->open_orders().empty());  // both cancelled on the way out
    ASSERT_FALSE(rig.fake->dms.empty());
    EXPECT_FALSE(rig.fake->dms.back().has_value());  // and only then the switch disarmed
}
