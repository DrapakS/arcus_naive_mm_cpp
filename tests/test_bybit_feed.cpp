#include <gtest/gtest.h>

#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "feeds/bybit.hpp"
#include "test_util.hpp"

using namespace nmm;
using namespace nmm::test;

// designated initializers name only the fields a test sets
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {

Json snapshot(int64_t u, const char* bid, const char* ask, int64_t cts) {
    JsonObject data {{"s", "BTCUSDT"}, {"b", JsonArray {JsonArray {Json(bid), Json("1.5")}}},
                     {"a", JsonArray {JsonArray {Json(ask), Json("2.0")}}}, {"u", u}, {"seq", 1}};
    return JsonObject {{"topic", "orderbook.1.BTCUSDT"}, {"ts", cts + 2}, {"type", "snapshot"}, {"data", data}, {"cts", cts}};
}

}  // namespace

TEST(BybitFeed, UrlPerCategory) {
    OrderBook book("bybit-linear", "BTCUSDT");
    BybitTickerFeed linear(book);
    EXPECT_EQ(linear.url, "wss://stream.bybit.com/v5/public/linear");
    EXPECT_EQ(linear.name, "bybit-linear");
    BybitTickerFeed spot(book, "spot", "BTCUSDT", "wss://stream.bybit.com/v5/public/spot");
    EXPECT_EQ(spot.url, "wss://stream.bybit.com/v5/public/spot");
    EXPECT_EQ(spot.topic, "orderbook.1.BTCUSDT");
    try {
        BybitTickerFeed inverse(book, "inverse");
        FAIL() << "no error";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("category"), std::string::npos);
    }
}

TEST(BybitFeed, EachSnapshotReplacesTheTopAndLaterCopiesAreDropped) {
    OrderBook book("bybit-linear", "BTCUSDT");
    BybitTickerFeed feed(book, "linear", "BTCUSDT", "wss://stream.bybit.com", FeedOptions {.connections = 2});
    auto ws = std::make_shared<DummyWs>();
    Link* a = &feed.links[0];
    Link* b = &feed.links[1];
    a->ws = b->ws = ws;  // both connected
    feed.on_message(J(R"({"success": true, "ret_msg": "", "op": "subscribe"})"), 0, a);  // the ack
    feed.on_message(snapshot(10, "100000.0", "100000.1", 5'000), 0, a);
    feed.on_message(snapshot(10, "100000.0", "100000.1", 5'000), 0, b);  // the same snapshot, later
    feed.on_message(snapshot(12, "100000.2", "100000.3", 5'001), 0, b);
    feed.on_message(snapshot(11, "99999.0", "99999.1", 5'001), 0, a);  // older than what the book has
    feed.on_message(J(R"({"success": true, "ret_msg": "pong", "op": "ping"})"), 0, a);
    Json empty = snapshot(13, "100000.4", "100000.5", 5'002);
    empty.as_object()["data"].as_object()["a"] = JsonArray {};
    feed.on_message(empty, 0, a);  // a side with nothing on it: the last full top stays
    EXPECT_EQ(book.best_bid(), Level(100'000.2, 1.5));
    EXPECT_EQ(book.best_ask(), Level(100'000.3, 2.0));
    EXPECT_TRUE(book.is_ready());
    EXPECT_EQ(book.seq, 12);
    EXPECT_EQ(book.exchange_ts_us, 5'001'000);
    EXPECT_EQ(a->wins, 1);
    EXPECT_EQ(b->wins, 1);
    // after a Bybit service restart the update id starts over at 1: newer by its engine time
    feed.on_message(snapshot(1, "100001.0", "100001.1", 6'000), 0, a);
    EXPECT_EQ(book.best_bid(), Level(100'001.0, 1.5));
    EXPECT_EQ(book.seq, 1);
}

TEST(BybitFeed, ASymbolBybitDoesNotListIsNamedInTheLog) {
    LogCapture logs("naive_mm.bybit-linear", logging::WARNING);
    OrderBook book("bybit-linear", "XAUUSDX");
    BybitTickerFeed feed(book, "linear", "XAUUSDX");
    feed.on_message(J(R"({"success": true, "ret_msg": "", "op": "subscribe"})"), 0, &feed.links[0]);
    EXPECT_TRUE(logs.messages().empty());
    feed.on_message(J(R"({"success": false, "ret_msg": "Invalid symbol :[orderbook.1.XAUUSDX]", "conn_id": "c1", "op": "subscribe"})"), 0,
                    &feed.links[0]);
    EXPECT_EQ(logs.messages(), (std::vector<std::string> {"subscribing to orderbook.1.XAUUSDX refused: Invalid symbol :[orderbook.1.XAUUSDX]"}));
}

TEST(BybitFeed, TopFollowsTheStreamOverTwoConnectionsAndPings) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeBybit server(ioc, {.n_books = 150}, {.clients = 2, .delays = {{1, 0.003}}});
    server.start();
    OrderBook book("bybit-linear", "BTCUSDT");
    BybitTickerFeed feed(book, "linear", "BTCUSDT", server.ws_url(), FeedOptions {.connections = 2}, 0.05);
    Task task = Task::spawn(feed.run());
    ASSERT_TRUE(run_until(ioc, [&] { return server.done; }, 10));
    const Json& last = server.sent.back();
    const Json& data = *jget(last, "data");
    ASSERT_TRUE(run_until(ioc, [&] { return book.seq == to_int(*jget(data, "u")); }));
    EXPECT_EQ(book.best_bid(), Level(to_float(jget(data, "b")->as_array()[0].as_array()[0]), 1.5));
    EXPECT_EQ(book.update_count, static_cast<int64_t>(server.sent.size()));  // each snapshot applied once
    EXPECT_EQ(feed.links[0].wins + feed.links[1].wins, static_cast<int64_t>(server.sent.size()));
    EXPECT_EQ(server.paths, (std::vector<std::string> {"/v5/public/linear", "/v5/public/linear"}));
    std::vector<Json> subs;
    for (auto& r : server.requests)
        if (jstr(r, "op") == "subscribe") subs.push_back(r);
    Json sub = J(R"({"op": "subscribe", "args": ["orderbook.1.BTCUSDT"]})");
    EXPECT_EQ(subs, (std::vector<Json> {sub, sub}));
    auto pings = [&] {
        return std::count_if(server.requests.begin(), server.requests.end(), [](const Json& r) { return jstr(r, "op") == "ping"; });
    };
    EXPECT_TRUE(run_until(ioc, [&] { return pings() >= 2; }));
    task.cancel();
    run_for(ioc, 0.02);
    server.close();
}

namespace {

// A server that sends one top, then only answers pings (control frames, or Bybit's
// app-level pong as text): the stream has stopped, the connection has not.
void stream_that_stops_while_pings_are_answered(bool pings_as_text) {
    // aiohttp restarted a receive timeout on every ping and pong, so with the
    // heartbeat (or Bybit's app-level pongs) a stopped stream was never noticed:
    // the top stayed frozen and "ready" until the exchange's daily cut.
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeServer server(ioc);
    int connects = 0;
    server.on_ws("/v5/public/linear", [&](std::shared_ptr<ServerWs> ws) {
        ++connects;
        std::weak_ptr<ServerWs> weak = ws;
        ws->on_message = [&, weak](const std::string& text) {
            auto conn = weak.lock();
            if (!conn) return;
            if (text.find("\"subscribe\"") != std::string::npos)
                conn->send_now(dump_json(snapshot(connects, "100000.0", "100000.1", 5'000)));
            else if (text.find("\"ping\"") != std::string::npos && pings_as_text)
                conn->send_now(R"({"success": true, "ret_msg": "pong", "op": "ping"})");
        };
    });
    server.start();
    OrderBook book("bybit-linear", "BTCUSDT");
    FeedOptions options;
    options.stale_timeout = 0.5;
    options.heartbeat = 0.1;
    options.reconnect_min = 0.05;
    BybitTickerFeed feed(book, "linear", "BTCUSDT", "ws://" + server.base(), options, 0.1);
    Task task = Task::spawn(feed.run());
    EXPECT_TRUE(run_until(ioc, [&] { return connects >= 2; }, 5));
    task.cancel();
    run_for(ioc, 0.02);
    server.close();
}

}  // namespace

TEST(BybitFeed, AStreamThatStopsWhilePingsAreAnsweredIsReconnected) { stream_that_stops_while_pings_are_answered(false); }

TEST(BybitFeed, AStreamThatStopsWhileAppPongsAreAnsweredIsReconnected) { stream_that_stops_while_pings_are_answered(true); }
