// test_review_19oct.py: the review of 19 Oct, small robustness fixes, one test each (the stuck
// orders log writer is with the other orders log tests, in test_main.cpp).
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "agent_helpers.hpp"
#include "fake_server.hpp"
#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "feeds/arcus.hpp"
#include "feeds/bybit.hpp"
#include "lgbm.hpp"
#include "trading/paper.hpp"
#include "trading/session.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

awaitable<void> fetch_into(ArcusBookFeed& feed, std::string& error) {
    try {
        co_await feed.fetch_snapshot();
        error = "no error";
    } catch (const std::exception& e) {
        error = e.what();
    }
}

}  // namespace

TEST(Review19Oct, ATreeWithoutNumLeavesIsAModelErrorNotACrash) {
    std::string text = read_file(std::string(NMM_SOURCE_DIR) + "/tests/data/lgbm_zero_missing.txt");
    auto at = text.find("num_leaves=");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("num_leaves=").size(), "leaves_were_here=");
    try {
        lgbm::parse_model(text);
        FAIL() << "no error";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("no num_leaves line"), std::string::npos) << e.what();
    }
}

TEST(Review19Oct, AnArcusSnapshotWithoutLastSequenceIdIsAFailedRequest) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeServer server(ioc);
    server.on_get("/v1/l2OrderBook/*", [](const HttpRequest&) { return HttpReply(200, R"({"error": "market not found"})"); });
    server.start();
    OrderBook book("arcus", "BTC-USD");
    ArcusBookFeed feed(book, "BTC-USD", "ws://" + server.base() + "/v1/ws", "http://" + server.base());
    std::string error;
    Task task = Task::spawn(fetch_into(feed, error));
    ASSERT_TRUE(run_until(ioc, [&] { return !error.empty(); }));
    // retried by the loader, never kept as the snapshot
    EXPECT_NE(error.find("without lastSequenceId"), std::string::npos) << error;
    server.close();
}

TEST(Review19Oct, ABybitTopWithoutCtsStillMovesByItsPushTime) {
    auto top = [](int64_t u, const char* bid, const char* ask, std::optional<int64_t> cts, int64_t ts) {
        JsonObject data {{"s", "BTCUSDT"}, {"b", JsonArray {JsonArray {Json(bid), Json("1.5")}}},
                         {"a", JsonArray {JsonArray {Json(ask), Json("2.0")}}}, {"u", u}, {"seq", 1}};
        JsonObject msg {{"topic", "orderbook.1.BTCUSDT"}, {"ts", ts}, {"type", "snapshot"}, {"data", data}};
        if (cts) msg["cts"] = *cts;
        return Json(msg);
    };
    OrderBook book("bybit-linear", "BTCUSDT");
    BybitTickerFeed feed(book);
    feed.on_message(top(10, "100000.0", "100000.1", 5'000, 5'002), 0, &feed.links[0]);
    feed.on_message(top(11, "100000.5", "100000.6", std::nullopt, 5'010), 0, &feed.links[0]);  // no engine time, after frames with it
    EXPECT_EQ(book.best_bid(), Level(100'000.5, 1.5));
    EXPECT_EQ(book.seq, 11);
}

TEST(Review19Oct, ATwoSidedTrendGuardTrippingBothSidesPullsAndLogsBoth) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Config config;
    config.trading.direction = "both";
    Agent agent(config);
    ASSERT_TRUE(agent.trend_guard && agent.futures_book);
    TrendGuard& guard = *agent.trend_guard;
    OrderBook& book = *agent.futures_book;
    FakeClock clock;
    guard.clock = clock.fn();
    for (double price : {100'000.0, 99'000.0, 99'650.0}) {  // the last: 35 bp under the high and 66 over the low
        clock.advance(1);
        book.apply_snapshot(lv(std::format("{:.1f}", price)), lv(std::format("{:.1f}", price + 0.1)), book.seq + 1);
        A::watch_moves(agent, guard, book, "trend: Binance futures", 0.0, true);
    }
    std::vector<std::string> trips;
    for (const auto& m : log.messages())
        if (starts_with(m, "!!! ")) trips.push_back(m);
    ASSERT_EQ(trips.size(), 2u) << join(log.messages(), "\n");  // the second buy trip extends the first pause
    EXPECT_TRUE(starts_with(trips[0], "!!! DROP trend: Binance futures mid 99000.05 is")) << trips[0];
    EXPECT_TRUE(starts_with(trips[1], "!!! RISE trend: Binance futures mid 99650.05 is")) << trips[1];
    EXPECT_NE(trips[1].find("over its low"), std::string::npos) << trips[1];
    EXPECT_TRUE(guard.paused("SELL"));
    EXPECT_TRUE(A::resume_timers(agent).count({&guard, "SELL"}));  // its end timed too
}

TEST(Review19Oct, AClockSteppedBackDoesNotFreezeTheSession) {
    Json rth = J(R"({"startSecondsOfDay": 14400, "endSecondsOfDay": 72000, "timezone": "America/New_York", "isOvernight": false})");
    double now = 1'792'195'200.0;  // Friday 16 Oct 2026 20:00 New York: closed
    SessionClock clock(*Hours::from_json(&rth), false, [&now] { return now; });
    EXPECT_TRUE(clock.outside());
    now -= 3600;  // NTP steps the clock back an hour: 19:00, open
    EXPECT_FALSE(clock.outside());
}

TEST(Review19Oct, ThePaperGatewayForgetsAnOrderOnceItIsDone) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    PaperGateway gw(btc_spec(), "ws://unused");
    gw.ready = true;
    OrderPtr buy = gw.place("BUY", Decimal(99900), Decimal("0.001"));
    gw.on_message(J(R"({"type": "channel_data", "channel": "trades", "id": "BTC-USD", "contents": [{"price": "99900", "size": "1"}]})"));
    EXPECT_EQ(buy->state, OrderState::FILLED);
    OrderPtr sell = gw.place("SELL", Decimal(100100), Decimal("0.001"));
    EXPECT_EQ(gw.placements_kept(), 1u);
    gw.cancel(sell);
    run_for(ioc, 0.05);
    EXPECT_EQ(sell->state, OrderState::CANCELED);
    EXPECT_EQ(gw.placements_kept(), 0u);
}
