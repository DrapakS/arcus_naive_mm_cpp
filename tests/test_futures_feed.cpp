#include <gtest/gtest.h>

#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "feeds/binance_futures.hpp"
#include "test_util.hpp"

using namespace nmm;
using namespace nmm::test;

// designated initializers name only the fields a test sets
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {

Json ticker(int64_t u, const char* bid, const char* ask, bool wrapped = true) {
    JsonObject data {{"e", "bookTicker"}, {"u", u}, {"s", "BTCUSDT"}, {"b", bid}, {"B", "1.5"}, {"a", ask}, {"A", "2.0"},
                     {"E", int64_t {1'700'000'000'000} + u}};
    if (!wrapped) return data;
    return JsonObject {{"stream", "btcusdt@bookTicker"}, {"data", data}};
}

}  // namespace

TEST(FuturesFeed, EachTickerReplacesTheTopAndLaterCopiesAreDropped) {
    OrderBook book("binance-futures", "BTCUSDT");
    BinanceFuturesTickerFeed feed(book, "BTCUSDT", "wss://fstream.binance.com", FeedOptions {.connections = 2});
    auto ws = std::make_shared<DummyWs>();
    Link* a = &feed.links[0];
    Link* b = &feed.links[1];
    a->ws = b->ws = ws;  // both connected
    EXPECT_EQ(feed.url, "wss://fstream.binance.com/public/stream?streams=btcusdt@bookTicker");
    for (const char* given : {"wss://fstream.binance.com/public", "wss://fstream.binance.com/stream", "wss://fstream.binance.com/"})
        EXPECT_EQ(BinanceFuturesTickerFeed(book, "BTCUSDT", given).url, feed.url);
    feed.on_message(ticker(10, "100000.0", "100000.1"), 0, a);
    feed.on_message(ticker(10, "100000.0", "100000.1"), 0, b);  // the same ticker, later
    feed.on_message(ticker(12, "100000.2", "100000.3", false), 0, b);  // raw stream format
    feed.on_message(ticker(11, "99999.0", "99999.1"), 0, a);  // older than what the book has
    feed.on_message(J(R"({"result": null, "id": 1})"), 0, a);  // not a ticker
    EXPECT_EQ(book.best_bid(), Level(100'000.2, 1.5));
    EXPECT_EQ(book.best_ask(), Level(100'000.3, 2.0));
    EXPECT_TRUE(book.is_ready());
    EXPECT_EQ(book.seq, 12);
    EXPECT_EQ(book.exchange_ts_us, (1'700'000'000'000 + 12) * 1000);
    EXPECT_EQ(a->wins, 1);
    EXPECT_EQ(b->wins, 1);
}

TEST(FuturesFeed, TopFollowsTheStreamOverTwoConnections) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeBinanceFutures server(ioc, {.n_tickers = 150}, {.clients = 2, .delays = {{1, 0.003}}});
    server.start();
    OrderBook book("binance-futures", "BTCUSDT");
    BinanceFuturesTickerFeed feed(book, "BTCUSDT", server.ws_url(), FeedOptions {.connections = 2});
    Task task = Task::spawn(feed.run());
    ASSERT_TRUE(run_until(ioc, [&] { return server.done; }, 10));
    const Json& last = server.sent.back();
    ASSERT_TRUE(run_until(ioc, [&] { return book.seq == to_int(*jget(last, "u")); }));
    EXPECT_EQ(book.best_bid(), Level(to_float(*jget(last, "b")), 1.5));
    EXPECT_EQ(book.best_ask(), Level(to_float(*jget(last, "a")), 2.0));
    EXPECT_EQ(book.update_count, static_cast<int64_t>(server.sent.size()));  // each ticker applied once
    EXPECT_EQ(feed.links[0].wins + feed.links[1].wins, static_cast<int64_t>(server.sent.size()));
    task.cancel();
    run_for(ioc, 0.02);
    server.close();
}

TEST(FuturesFeed, ALinkWithDataStaysPastItsStaleTimeoutAndOneGoneSilentIsDropped) {
    // The stale watch fires about once per timeout: a link still bringing data is kept,
    // one that stopped (its connection open, pings answered) is reconnected.
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeServer server(ioc);
    int connects = 0;
    bool quiet = false;
    std::vector<Task> senders;
    server.on_ws("/public/stream", [&](std::shared_ptr<ServerWs> ws) {
        ++connects;
        int64_t first = 100 * connects;
        senders.push_back(Task::spawn([](std::shared_ptr<ServerWs> ws, int64_t u, bool& quiet) -> awaitable<void> {
            while (!quiet && !ws->closed()) {
                ++u;
                ws->send_now(dump_json(ticker(u, std::format("{}.0", 100'000 + u).c_str(), std::format("{}.1", 100'000 + u).c_str())));
                co_await sleep(0.02);
            }  // silent from here on, the connection kept open
        }(ws, first, quiet)));
    });
    server.start();
    OrderBook book("binance-futures", "BTCUSDT");
    FeedOptions options;
    options.stale_timeout = 0.15;
    options.heartbeat = 0.05;
    options.reconnect_min = 0.05;
    BinanceFuturesTickerFeed feed(book, "BTCUSDT", "ws://" + server.base(), options);
    Task task = Task::spawn(feed.run());
    run_for(ioc, 0.6);  // four timeouts of data
    EXPECT_EQ(connects, 1);
    EXPECT_GT(book.seq, 110);
    quiet = true;
    EXPECT_TRUE(run_until(ioc, [&] { return connects >= 2; }, 5));  // dropped and reconnected
    task.cancel();
    for (auto& s : senders) s.cancel();
    run_for(ioc, 0.02);
    server.close();
}
