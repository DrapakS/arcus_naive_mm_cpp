#include <gtest/gtest.h>

#include <tuple>

#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "feeds/binance_trades.hpp"
#include "test_util.hpp"

using namespace nmm;
using namespace nmm::test;

// designated initializers name only the fields a test sets
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {

Json trade(int64_t a, const char* price, int64_t t_ms = 1'700'000'000'000, bool wrapped = true) {
    JsonObject data {{"e", "aggTrade"}, {"E", t_ms + 1}, {"s", "BTCUSDT"}, {"a", a}, {"p", price}, {"q", "0.01"}, {"f", a},
                     {"l", a}, {"T", t_ms}, {"m", false}, {"M", true}};
    if (!wrapped) return data;
    return JsonObject {{"stream", "btcusdt@aggTrade"}, {"data", data}};
}

}  // namespace

TEST(TradesFeed, Url) {
    BinanceTradesFeed feed("ETHUSDT");
    EXPECT_EQ(feed.url, "wss://stream.binance.com:9443/stream?streams=ethusdt@aggTrade");
    EXPECT_EQ(feed.book, nullptr);
    for (const char* given : {"wss://stream.binance.com:9443/ws", "wss://stream.binance.com:9443/stream", "wss://stream.binance.com:9443/"})
        EXPECT_EQ(BinanceTradesFeed("ETHUSDT", given).url, feed.url);
}

TEST(TradesFeed, EachTradeIsPassedOnOnceAndLaterCopiesAreDropped) {
    std::vector<std::tuple<double, int64_t, int64_t>> seen;
    int resets = 0;
    BinanceTradesFeed feed("BTCUSDT", "wss://stream.binance.com:9443",
                           [&](double p, int64_t t, int64_t r) { seen.emplace_back(p, t, r); }, [&] { ++resets; },
                           FeedOptions {.connections = 2});
    auto ws = std::make_shared<DummyWs>();
    Link* a = &feed.links[0];
    Link* b = &feed.links[1];
    a->ws = b->ws = ws;  // both connected
    feed.on_message(trade(10, "100000.00"), 1'700'000'000'003'000, a);
    feed.on_message(trade(10, "100000.00"), 1'700'000'000'004'000, b);  // the same trade, later
    feed.on_message(trade(12, "99999.50", 1'700'000'000'000, false), 1'700'000'000'005'000, b);  // raw stream format
    feed.on_message(trade(11, "99999.90"), 1'700'000'000'006'000, a);  // older than the newest passed on
    feed.on_message(J(R"({"result": null, "id": 1})"), 0, a);  // not a trade
    EXPECT_EQ(seen, (std::vector<std::tuple<double, int64_t, int64_t>> {{100'000.0, 1'700'000'000'000'000, 1'700'000'000'003'000},
                                                                       {99'999.5, 1'700'000'000'000'000, 1'700'000'000'005'000}}));
    EXPECT_EQ(a->wins, 1);
    EXPECT_EQ(b->wins, 1);
    EXPECT_EQ(feed.trade_count, 2);
    EXPECT_EQ(feed.delays.last, 5.0);
    EXPECT_EQ(*feed.delays.take(), "p50 5.0 / p99 5.0 / max 5.0 ms (n 2)");
    EXPECT_NE(feed.status().find("trades 2, last read 5ms after it"), std::string::npos) << feed.status();
    EXPECT_NE(feed.status().find("links 2/2 first 50/50%"), std::string::npos) << feed.status();
    a->ws = nullptr;
    feed.on_disconnected(a);  // the other link carries on: nothing lost
    EXPECT_EQ(resets, 0);
    EXPECT_EQ(feed.last_id, 12);
    b->ws = nullptr;
    feed.on_disconnected(b);  // both down: the trades in between are lost
    EXPECT_EQ(resets, 1);
    EXPECT_EQ(feed.last_id, -1);
}

TEST(TradesFeed, TradesFollowTheStreamOverTwoConnections) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeBinanceTrades server(ioc, {.n_trades = 150}, {.clients = 2, .delays = {{1, 0.003}}});
    server.start();
    std::vector<double> seen;
    BinanceTradesFeed feed("BTCUSDT", server.ws_url(), [&](double p, int64_t, int64_t) { seen.push_back(p); }, {},
                           FeedOptions {.connections = 2});
    Task task = Task::spawn(feed.run());
    ASSERT_TRUE(run_until(ioc, [&] { return server.done; }, 10));
    ASSERT_TRUE(run_until(ioc, [&] { return seen.size() == server.sent.size(); }));
    EXPECT_EQ(server.streams, (std::vector<std::string> {"btcusdt@aggTrade"}));
    std::vector<double> sent;
    for (auto& t : server.sent) sent.push_back(to_float(*jget(t, "p")));
    EXPECT_EQ(seen, sent);  // each once, in order
    EXPECT_EQ(feed.links[0].wins + feed.links[1].wins, static_cast<int64_t>(server.sent.size()));
    task.cancel();
    run_for(ioc, 0.02);
    server.close();
}
