// test_slow_links.py: a stream on several connections: each copy timed against the first copy of
// the same item, and a connection that stays late reconnected (19 Oct: one Arcus trades
// connection 74 ms behind another for an hour after a reconnect).
#include <gtest/gtest.h>

#include "config.hpp"
#include "fake_server.hpp"
#include "feed_helpers.hpp"
#include "feeds/binance_futures.hpp"
#include "gateway_rig.hpp"
#include "race.hpp"
#include "test_util.hpp"
#include "trading/paper.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {

// The tests send copies faster than a link samples them.
class SlowLinks : public testing::Test {
protected:
    Patch every_copy {SAMPLE_EVERY_S, 0.0};
};

// A connection that notes its close (the feed's way to drop a late link).
class ClosingWs : public DummyWs {
public:
    void close() override { closed = true; }
    bool closed = false;
};

Json ticker(int64_t u, double price = 100.0) {
    return JsonObject {{"stream", "btcusdt@bookTicker"},
                       {"data", JsonObject {{"u", u}, {"E", 1}, {"b", std::format("{:.1f}", price)}, {"B", "1"},
                                            {"a", std::format("{:.1f}", price + 1)}, {"A", "1"}}}};
}

Json trades_frame(const std::string& trade_id, const std::string& price = "99900", const std::string& size = "0.0003") {
    JsonObject trade {{"price", price}, {"size", size}, {"side", "SELL"}, {"tradeId", trade_id}};
    return JsonObject {{"type", "channel_data"}, {"channel", "trades"}, {"id", "BTC-USD"}, {"contents", JsonArray {trade}}};
}

MarketSpec btc() {
    return MarketSpec::from_json(J(R"({"marketId": 1, "marketDisplayName": "BTC-USD", "tickSize": "0.1",
        "stepSize": "0.00000001", "minOrderSize": "0.0001", "minOrderNotional": "5", "maxOrderSize": "10000"})"));
}

}  // namespace

TEST(LinkLag, AFastStreamKeepsOneCopyALinkEach20Ms) {
    LinkLag lag;
    for (int i = 0; i < 1000; ++i) lag.add(5.0);  // a burst: within microseconds
    EXPECT_EQ(lag.samples.size(), 1u);
}

TEST_F(SlowLinks, EachCopyIsTimedAgainstTheFirstCopyOfTheSameItem) {
    FirstCopies copies(3);
    EXPECT_EQ(copies.arrived(1, 1'000), std::make_pair(true, 0.0));
    EXPECT_EQ(copies.arrived(1, 75'000), std::make_pair(false, 74.0));
    EXPECT_EQ(copies.arrived(1, 500), std::make_pair(false, 0.0));  // never below 0
    for (int64_t key : {2, 3, 4}) copies.arrived(key, 0);
    EXPECT_EQ(copies.arrived(1, 9'000), std::make_pair(true, 0.0));  // forgotten: 3 newer ones since
}

TEST_F(SlowLinks, ALinkIsJudgedOnTheMedianOfAMinuteOnceSettledAndNotAgainSoon) {
    LinkLag lag;
    double now = monotonic();
    lag.connected_at = now - SLOW_LINK_SETTLE_S - 1;
    for (size_t i = 0; i + 1 < SLOW_LINK_MIN_SAMPLES; ++i) lag.add(74.0);
    EXPECT_FALSE(lag.judge(20.0, now));  // too few copies
    lag.add(74.0);
    EXPECT_EQ(lag.judge(20.0, now), std::make_pair(74.0, SLOW_LINK_MIN_SAMPLES));
    EXPECT_FALSE(lag.judge(0.0, now));  // off
    EXPECT_FALSE(lag.judge(80.0, now));  // under the threshold
    EXPECT_FALSE(lag.judge(20.0, now + SLOW_LINK_WINDOW_S + 1));  // all older than the window
    lag.dropped_for_slowness(now);
    EXPECT_FALSE(lag.judge(20.0, now + 1));  // reconnected for it just now
    EXPECT_EQ(lag.slow_drops, 1);
    lag.connected_at = now - SLOW_LINK_SETTLE_S - 1;
    EXPECT_FALSE(lag.judge(20.0, now));  // ... and the gap holds even then
}

TEST_F(SlowLinks, AFeedLinkThatStaysLateIsReconnectedWhileTheOtherIsUp) {
    LogCapture log("naive_mm.binance-futures", logging::INFO);
    OrderBook book("binance-futures", "BTCUSDT");
    BinanceFuturesTickerFeed feed(book, "BTCUSDT", "wss://fstream.binance.com", FeedOptions {.connections = 2, .slow_link_ms = 20.0});
    Link& fast = feed.links[0];
    Link& slow = feed.links[1];
    auto fast_ws = std::make_shared<ClosingWs>(), slow_ws = std::make_shared<ClosingWs>();
    fast.ws = fast_ws;
    slow.ws = slow_ws;
    for (auto& link : feed.links) link.lag.connected_at -= SLOW_LINK_SETTLE_S + 1;
    for (int64_t u = 1; u <= 30; ++u) {
        feed.on_message(ticker(u), 1'000'000 + u * 1'000, &fast);
        feed.on_message(ticker(u), 1'074'000 + u * 1'000, &slow);  // every copy 74 ms after the other's
    }
    EXPECT_EQ(fast.wins, 30);
    EXPECT_EQ(slow.wins, 0);
    EXPECT_EQ(book.seq, 30);
    EXPECT_EQ(fast.lag.median()->first, 0.0);
    EXPECT_EQ(slow.lag.median()->first, 74.0);
    EXPECT_EQ(feed.check_links(), &slow);
    EXPECT_TRUE(slow_ws->closed);
    EXPECT_FALSE(fast_ws->closed);
    EXPECT_TRUE(slow.slow_drop);
    EXPECT_EQ(slow.lag.slow_drops, 1);
    EXPECT_NE(join(log.messages(), "\n").find(
                  "!!! link 2/2 slow: its copies 74 ms behind the first at the median over the last 120 s (n 30, over 20 ms)"),
              std::string::npos)
        << join(log.messages(), "\n");
    EXPECT_EQ(feed.check_links(), nullptr);  // being closed already
    slow.slow_drop = false;
    slow.ws.reset();
    EXPECT_EQ(feed.check_links(), nullptr);  // the fast one is never judged against a link that is down
    EXPECT_EQ(feed.links_status(), "links 1/2 first 100/0% behind 0/74ms");
}

TEST_F(SlowLinks, OneLinkOrSlowLinkMs0TimesNothing) {
    OrderBook book("binance-futures", "BTCUSDT");
    BinanceFuturesTickerFeed single(book, "BTCUSDT", "wss://fstream.binance.com", FeedOptions {.slow_link_ms = 20.0});
    single.on_message(ticker(1), 1'000, &single.links[0]);
    EXPECT_TRUE(single.links[0].lag.samples.empty());
    OrderBook other("binance-futures", "BTCUSDT");
    BinanceFuturesTickerFeed off(other, "BTCUSDT", "wss://fstream.binance.com", FeedOptions {.connections = 2});
    for (auto& link : off.links) {
        link.ws = std::make_shared<ClosingWs>();
        link.lag.connected_at = 0.0;
        for (int i = 0; i < 30; ++i) link.lag.add(500.0);
    }
    EXPECT_EQ(off.check_links(), nullptr);
}

TEST_F(SlowLinks, PaperTradesOnTwoConnectionsFillOnceByTheFirstCopy) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    PaperGateway gw(btc(), "ws://unused", {}, 2, 20.0);
    gw.ready = true;
    int seen = 0;
    gw.on_trades = [&](const JsonArray&) { ++seen; };
    Link& first = gw.links[0];
    Link& second = gw.links[1];
    OrderPtr buy = gw.place("BUY", Decimal(99900), Decimal("0.001"));
    run_for(ioc, 0.001);
    gw.on_message(trades_frame("1"), &second);  // the second connection is first this time
    gw.on_message(trades_frame("1"), &first);
    EXPECT_EQ(buy->filled, Decimal("0.0003"));  // one fill, one frame to the take guard
    EXPECT_EQ(seen, 1);
    gw.on_message(trades_frame("2"), &first);
    EXPECT_EQ(buy->filled, Decimal("0.0006"));
    EXPECT_EQ(seen, 2);
    ASSERT_FALSE(second.lag.samples.empty());
    EXPECT_EQ(second.lag.samples.front().second, 0.0);
    EXPECT_EQ(first.lag.samples.size(), 2u);
}

// A server sending each trade on both connections; one connection ends: logged, the order stays
// (the other still brings the trades); none left: the orders are cancelled.
TEST_F(SlowLinks, PaperLogsAConnectionThatEndsAndCancelsOnlyWhenNoTradesConnectionIsLeft) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeServer server(ioc);
    std::vector<std::weak_ptr<ServerWs>> subscribers;
    server.on_ws("/v1/ws", [&](std::shared_ptr<ServerWs> ws) {
        std::weak_ptr<ServerWs> weak = ws;
        ws->on_message = [&, weak](const std::string& text) {
            if (text.find("\"subscribe\"") != std::string::npos) subscribers.push_back(weak);
        };
    });
    server.start();
    LogCapture log("naive_mm.paper", logging::INFO);
    PaperGateway gw(btc(), "ws://" + server.base() + "/v1/ws", {}, 2);
    Task task = Task::spawn(gw.run());
    ASSERT_TRUE(run_until(ioc, [&] { return subscribers.size() == 2 && gw.ready && gw.links[0].up() && gw.links[1].up(); }));
    OrderPtr buy = gw.place("BUY", Decimal(99900), Decimal("0.001"));
    if (auto ws = subscribers[0].lock()) ws->close();
    ASSERT_TRUE(run_until(ioc, [&] { return gw.links[0].up() + gw.links[1].up() == 1; }));
    EXPECT_NE(join(log.messages(), "\n").find("trades connection closed by server (code="), std::string::npos) << join(log.messages(), "\n");
    EXPECT_EQ(buy->state, OrderState::OPEN);
    EXPECT_TRUE(gw.ready);
    std::string frame = dump_json(trades_frame("t1", "99900", "0.0004"));
    for (auto& s : subscribers)
        if (auto ws = s.lock()) ws->send_now(frame);
    ASSERT_TRUE(run_until(ioc, [&] { return buy->filled == Decimal("0.0004"); }));
    for (auto& s : subscribers)
        if (auto ws = s.lock()) ws->close();
    ASSERT_TRUE(run_until(ioc, [&] { return !gw.ready; }));
    EXPECT_EQ(buy->state, OrderState::CANCELED);
    EXPECT_EQ(buy->reason, "trades feed lost");
    task.cancel();
    run_for(ioc, 0.02);
    server.close();
}

TEST_F(SlowLinks, LiveTradesComeOnMainAndTheLaneAndCountOnce) {
    Rig rig({.cancel_lanes = 1});
    ArcusGateway& gw = *rig.gw;
    int frames = 0;
    gw.on_trades = [&](const JsonArray&) { ++frames; };
    rig.quote();
    OrderPtr buy = gw.order();
    ASSERT_TRUE(buy);
    rig.fake->fill(buy->client_id, "0.0004", false);  // public trades only: on both connections
    ASSERT_TRUE(rig.wait([&] { return gw.main_conn.lag.samples.size() == 1 && gw.lanes[0]->lag.samples.size() == 1; }));
    EXPECT_EQ(buy->filled, Decimal("0.0004"));
    EXPECT_EQ(gw.position(), Decimal("0.0004"));
    EXPECT_EQ(gw.stats["fills"], 1);
    EXPECT_EQ(frames, 1);
    EXPECT_NE(gw.status().find("trades behind the first copy (ms) main"), std::string::npos) << gw.status();
}

TEST_F(SlowLinks, ALiveConnectionWhoseTradesStayLateIsReconnected) {
    GatewayLog log;
    Rig rig({.cancel_lanes = 1});
    ArcusGateway& gw = *rig.gw;
    gw.opt.slow_reconnect_ms = 50.0;
    gw.opt.slow_link_ms = 20.0;
    Conn& main = gw.main_conn;
    Conn& lane = *gw.lanes[0];
    double now = monotonic() + SLOW_LINK_SETTLE_S + 1;
    for (int i = 0; i < 30; ++i) {
        main.lag.add(0.0);
        lane.lag.add(74.0);
    }
    auto old = lane.ws;
    EXPECT_EQ(gw.check_slow(now), &lane);
    EXPECT_NE(log.text().find("!!! lane1 connection slow: public trades behind another connection's copies, median 74 ms over the "
                              "last 120 s (n 30, over 20 ms): reconnecting"),
              std::string::npos)
        << log.text();
    ASSERT_TRUE(rig.wait([&] { return lane.ws != nullptr && lane.ws != old; }));
    EXPECT_NE(main.ws, nullptr);
    EXPECT_TRUE(gw.ready);
    EXPECT_EQ(lane.lag.slow_drops, 1);
}

TEST(SlowLinkConfigs, ConfigsReconnectLateLinksAndTheMlArcusBookIsOnTwo) {
    for (const char* name : {"eth-ml-both", "btc-ml-both", "btc-both", "nvda-both"}) {
        Config cfg = load_config(std::format("{}/config.{}.toml", NMM_SOURCE_DIR, name));
        EXPECT_EQ(cfg.agent.slow_link_ms, 30.0) << name;
        EXPECT_EQ(cfg.trading.slow_reconnect_ms, 50.0) << name;
        EXPECT_EQ(cfg.arcus.connections, 2) << name;
    }
}
