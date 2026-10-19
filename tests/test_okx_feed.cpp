// test_okx_feed.py: the OKX swap's best bid/ask, one more input of the price guard.
#include <gtest/gtest.h>

#include "agent_helpers.hpp"
#include "fake_server.hpp"
#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "feeds/okx.hpp"
#include "test_util.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

Json okx_top(int64_t seq, const std::string& bid, const std::string& ask, int64_t ts, const std::string& inst = "BTC-USDT-SWAP") {
    JsonObject top {{"asks", JsonArray {JsonArray {Json(ask), Json("20"), Json("0"), Json("3")}}},
                    {"bids", JsonArray {JsonArray {Json(bid), Json("15"), Json("0"), Json("2")}}},
                    {"ts", std::to_string(ts)},
                    {"seqId", seq}};
    return JsonObject {{"arg", JsonObject {{"channel", "bbo-tbt"}, {"instId", inst}}}, {"data", JsonArray {top}}};
}

}  // namespace

TEST(OkxFeed, UrlAndInstrument) {
    OrderBook book("okx-swap", "ETH-USDT-SWAP");
    OkxTickerFeed feed(book, "eth-usdt-swap");
    EXPECT_EQ(feed.url, "wss://ws.okx.com:8443/ws/v5/public");
    EXPECT_EQ(feed.arg, J(R"({"channel": "bbo-tbt", "instId": "ETH-USDT-SWAP"})"));
    OrderBook other_book("okx-swap", "BTC-USDT-SWAP");
    OkxTickerFeed other(other_book, "BTC-USDT-SWAP", "wss://ws.okx.com/ws/v5/public");
    EXPECT_EQ(other.url, "wss://ws.okx.com/ws/v5/public");
}

TEST(OkxFeed, EachTopReplacesTheBookAndLaterCopiesAreDropped) {
    OrderBook book("okx-swap", "BTC-USDT-SWAP");
    OkxTickerFeed feed(book, "BTC-USDT-SWAP", "wss://ws.okx.com:8443", FeedOptions {.connections = 2});
    auto ws = std::make_shared<DummyWs>();
    Link* a = &feed.links[0];
    Link* b = &feed.links[1];
    a->ws = b->ws = ws;
    feed.on_message(JsonObject {{"event", "subscribe"}, {"arg", feed.arg}, {"connId", "c1"}}, 0, a);  // the ack
    feed.on_message(okx_top(10, "100000.0", "100000.1", 5'000), 0, a);
    feed.on_message(okx_top(10, "100000.0", "100000.1", 5'000), 0, b);  // the same top, later
    feed.on_message(okx_top(12, "100000.2", "100000.3", 5'001), 0, b);
    feed.on_message(okx_top(11, "99999.0", "99999.1", 5'001), 0, a);  // older than the book
    feed.on_message(okx_top(13, "1", "2", 5'002, "ETH-USDT-SWAP"), 0, a);  // another instrument
    Json empty = okx_top(14, "100000.4", "100000.5", 5'002);
    empty.as_object()["data"].as_array()[0].as_object()["asks"] = JsonArray {};
    feed.on_message(empty, 0, a);  // one side empty: the last full top stays
    EXPECT_EQ(book.best_bid(), Level(100'000.2, 15.0));
    EXPECT_EQ(book.best_ask(), Level(100'000.3, 20.0));
    EXPECT_TRUE(book.is_ready());
    EXPECT_EQ(book.seq, 12);
    EXPECT_EQ(book.exchange_ts_us, 5'001'000);
    EXPECT_EQ(a->wins, 1);
    EXPECT_EQ(b->wins, 1);
    // after an OKX upgrade the ids start lower: newer by their time
    feed.on_message(okx_top(3, "100001.0", "100001.1", 6'000), 0, a);
    EXPECT_EQ(book.best_bid(), Level(100'001.0, 15.0));
    EXPECT_EQ(book.seq, 3);
    EXPECT_FALSE(feed.carries_data(JsonObject {{"event", "subscribe"}, {"arg", feed.arg}}));
    EXPECT_TRUE(feed.carries_data(okx_top(15, "1", "2", 7'000)));
    EXPECT_TRUE(feed.skips("pong"));
    EXPECT_FALSE(feed.skips(dump_json(okx_top(15, "1", "2", 7'000))));
}

TEST(OkxFeed, AnInstrumentOkxDoesNotListIsNamedInTheLog) {
    LogCapture logs("naive_mm.okx", logging::WARNING);
    OrderBook book("okx-swap", "XYZ-USDT-SWAP");
    OkxTickerFeed feed(book, "XYZ-USDT-SWAP");
    feed.on_message(J(R"({"event": "error", "code": "60018", "msg": "Wrong URL or channel:bbo-tbt,instId:XYZ-USDT-SWAP"})"), 0,
                    &feed.links[0]);
    EXPECT_EQ(logs.messages(),
              (std::vector<std::string> {"subscribing to XYZ-USDT-SWAP refused: Wrong URL or channel:bbo-tbt,instId:XYZ-USDT-SWAP"}));
}

// An OKX-like server: answers a text ping with a bare pong; sends three tops a connection, then
// only pongs. The pongs are no data: the stopped stream is reconnected.
TEST(OkxFeed, PongsAreNoDataAndAStreamThatStopsIsReconnected) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeServer server(ioc);
    int connects = 0, pings = 0, seq = 0;
    server.on_ws("/ws/v5/public", [&](std::shared_ptr<ServerWs> ws) {
        ++connects;
        std::weak_ptr<ServerWs> weak = ws;
        ws->on_message = [&, weak](const std::string& text) {
            auto conn = weak.lock();
            if (!conn) return;
            if (text == "ping") {
                ++pings;
                conn->send_now("pong");
            } else if (text.find("\"subscribe\"") != std::string::npos) {
                conn->send_now(R"({"event": "subscribe", "arg": {"channel": "bbo-tbt", "instId": "BTC-USDT-SWAP"}})");
                for (int i = 0; i < 3; ++i) {
                    ++seq;
                    conn->send_now(dump_json(okx_top(seq, std::format("{}.0", 100'000 + seq), std::format("{}.1", 100'000 + seq), 1'000 + seq)));
                }
            }
        };
    });
    server.start();
    OrderBook book("okx-swap", "BTC-USDT-SWAP");
    FeedOptions options;
    options.stale_timeout = 0.5;
    options.heartbeat = 0.1;
    options.reconnect_min = 0.05;
    OkxTickerFeed feed(book, "BTC-USDT-SWAP", "ws://" + server.base(), options, 0.05);
    Task task = Task::spawn(feed.run());
    EXPECT_TRUE(run_until(ioc, [&] { return pings >= 2 && book.seq == 3; }));
    EXPECT_EQ(book.best_bid(), Level(100'003.0, 15.0));
    EXPECT_EQ(feed.message_count, 4);  // the ack and three tops: no pong
    EXPECT_TRUE(run_until(ioc, [&] { return connects >= 2; }, 5));  // pongs kept coming, the tops did not
    task.cancel();
    run_for(ioc, 0.02);
    server.close();
}

namespace {

// test_okx_feed._okx: the OKX top is now `bid` / `bid` + 0.1, read 40 ms after OKX stamped it.
void okx(Agent& agent, const std::string& bid) {
    OrderBook& book = *agent.okx_book;
    book.apply_snapshot(lv(bid), lv(std::format("{:.1f}", std::stod(bid) + 0.1)), book.seq + 1, 5'000'000);
    book.exchange_ts_us = book.local_ts_us - 40'000;
    agent.on_book_update(book);
}

}  // namespace

TEST(OkxFeed, OkxDropPullsABuyAndRisePullsASell) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    {
        Bot b = guarded_agent("BUY", true);
        Agent& a = *b.agent;
        okx(a, "100100");
        b.clock.advance(0.01);
        okx(a, "100100");
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));  // OKX frames alone change nothing
        okx(a, "100080");  // 2 bp under the OKX mean
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));
        auto messages = log.messages();
        EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& m) {
            return starts_with(m, "!!! DROP OKX swap bid 100080.00 is 2.00 bp under its mean 100100.00 over the last") &&
                   m.find("(read 40 ms after the exchange's event)") != std::string::npos;
        })) << join(messages, "\n");
        EXPECT_TRUE(a.strategy->paused("BUY"));
        EXPECT_FALSE(a.strategy->paused("SELL"));
    }
    {
        Bot b = guarded_agent("SELL", true);
        Agent& a = *b.agent;
        okx(a, "100100");
        b.clock.advance(0.01);
        okx(a, "100130");  // 3 bp over
        EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
    }
}

TEST(OkxFeed, OkxSettings) {
    Loop loop;
    Config config;
    EXPECT_FALSE(config.okx.enabled);
    {
        Agent off(config);  // off by default
        EXPECT_FALSE(off.okx_book);
        EXPECT_FALSE(off.okx_guard);
    }
    config.okx.enabled = true;
    {
        Agent on(config);
        ASSERT_TRUE(on.okx_guard);
        EXPECT_EQ(on.okx_book->symbol, "BTC-USDT-SWAP");
        EXPECT_DOUBLE_EQ(on.okx_guard->window_s, 0.2);
        EXPECT_DOUBLE_EQ(on.okx_guard->drawdown, 0.00002);
        EXPECT_DOUBLE_EQ(on.okx_guard->pause_s, 0.3);
    }
    config.okx.drawdown = 0;
    {
        Agent feed_only(config);
        EXPECT_FALSE(feed_only.okx_guard);  // the feed stays, for the price lines
        EXPECT_TRUE(feed_only.okx_book);
    }
    TempFile f;
    for (const char* bad : {"connections = 5", "drawdown = 1", "window_ms = 0", "inst_id = \"ETH-USDT-SWAP\"", "inst_id = \"BTC-USDT\"",
                            "inst_id = \"BTC-USD-SWAP-X\""}) {
        f.write(std::format("[okx]\nenabled = true\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "okx");
    }
    f.write("[arcus]\nmarket = \"ETH-USD\"\n[binance]\nsymbol = \"ETHUSDT\"\n[futures]\nsymbol = \"ETHUSDT\"\n"
            "[bybit]\nsymbol = \"ETHUSDT\"\n[okx]\nenabled = true\ninst_id = \"ETH-USDT-SWAP\"\n");
    EXPECT_EQ(load_config(f.written).okx.inst_id, "ETH-USDT-SWAP");
    f.write("[okx]\nenabled = true\ninst_id = \"ETH-USDT-SWAP\"\n");
    EXPECT_EQ(load_error(f.written),
              "okx.inst_id = 'ETH-USDT-SWAP' is not the BTC perpetual against the dollar (\"BTC-USDT-SWAP\"), but arcus.market = 'BTC-USD'");
}
