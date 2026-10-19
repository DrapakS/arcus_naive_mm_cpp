// C++ only: each feed measures how far behind the first copy its links deliver an update, and
// reconnects a link that keeps running behind; the Arcus trades of the paper gateway come over
// several links, each trade once.
#include <gtest/gtest.h>

#include "fake_server.hpp"
#include "feed_helpers.hpp"
#include "feeds/arcus_trades.hpp"
#include "feeds/binance_futures.hpp"
#include "test_util.hpp"
#include "trading/paper.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

// A connection that counts its closes (the feed's way to drop a slow link).
class ClosingWs : public DummyWs {
public:
    void close() override { ++closes; }
    int closes = 0;
};

Json ticker(int64_t u, const std::string& bid) {
    return JsonObject {{"e", "bookTicker"}, {"u", u}, {"b", bid}, {"B", "1"}, {"a", std::format("{:.1f}", std::stod(bid) + 0.1)}, {"A", "1"}};
}

struct TwoLinks {
    OrderBook book {"binance-futures", "BTCUSDT"};
    FakeClock clock;
    std::shared_ptr<ClosingWs> wa = std::make_shared<ClosingWs>(), wb = std::make_shared<ClosingWs>();
    BinanceFuturesTickerFeed feed;
    Link& a;
    Link& b;
    explicit TwoLinks(double slow_link_ms = 25.0)
        : feed(book, "BTCUSDT", "wss://fstream.binance.com", FeedOptions {.connections = 2, .slow_link_ms = slow_link_ms}),
          a(feed.links[0]), b(feed.links[1]) {
        feed.clock = clock.fn();
        a.ws = wa;
        b.ws = wb;
        a.connected_at = b.connected_at = clock();
    }
    // `n` updates, one every 100 ms: `first` delivers each, the other `later_ms` after it
    void run(int n, Link& first, Link& other, double later_ms, int64_t& u) {
        for (int i = 0; i < n; ++i) {
            ++u;
            feed.on_message(ticker(u, "100000.0"), 0, &first);
            clock.advance(later_ms / 1000);
            feed.on_message(ticker(u, "100000.0"), 0, &other);
            clock.advance(0.1 - later_ms / 1000);
        }
    }
};

}  // namespace

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

TEST(SlowLinks, AFeedMeasuresHowFarBehindEachLinkRuns) {
    TwoLinks t;
    int64_t u = 0;
    t.run(10, t.a, t.b, 7.0, u);
    EXPECT_EQ(t.feed.behind(t.a), 0.0);
    EXPECT_NEAR(*t.feed.behind(t.b), 7.0, 1e-6);
    EXPECT_EQ(t.a.wins, 10);  // the first copies moved the book
    EXPECT_EQ(t.b.wins, 0);
    EXPECT_EQ(t.feed.links_status(), "links 2/2 first 100/0% behind 0/7 ms");
}

TEST(SlowLinks, ALinkThatKeepsRunningBehindIsReconnectedOnceTheOthersCarryTheStream) {
    LogCapture log("naive_mm.binance-futures", logging::WARNING);
    TwoLinks t;
    int64_t u = 0;
    t.run(100, t.a, t.b, 74.0, u);  // 10 s: connected too briefly to judge
    EXPECT_EQ(t.wb->closes, 0);
    t.run(200, t.a, t.b, 74.0, u);  // 30 s
    EXPECT_EQ(t.wb->closes, 1);
    EXPECT_EQ(t.wa->closes, 0);
    EXPECT_EQ(t.b.slow_reconnects, 1);
    ASSERT_EQ(log.messages().size(), 1u);
    EXPECT_TRUE(starts_with(log.messages()[0], "74 ms behind the other links over the last ")) << log.messages()[0];
    EXPECT_TRUE(ends_with(log.messages()[0], "updates (median): reconnecting [link 2/2]")) << log.messages()[0];
    // (the test link keeps delivering after its close: a real one reconnects, its window empty)
    EXPECT_EQ(t.feed.links_status(), "links 2/2 first 100/0% behind 0/74 ms slow reconnects 1");
}

TEST(SlowLinks, LinksTakingTurnsAreNeverReconnected) {
    TwoLinks t;
    int64_t u = 0;
    for (int i = 0; i < 20; ++i) {  // 60 s, each link first half the time, the other up to 20 ms after it
        t.run(15, t.a, t.b, 20.0, u);
        t.run(15, t.b, t.a, 3.0, u);
    }
    EXPECT_EQ(t.wa->closes + t.wb->closes, 0);
}

TEST(SlowLinks, TheLastLinkUpIsKeptHoweverLate) {
    TwoLinks t;
    int64_t u = 0;
    t.run(300, t.a, t.b, 74.0, u);
    ASSERT_EQ(t.wb->closes, 1);
    t.b.ws.reset();  // b is down: a carries the stream, and is never dropped for being "behind" nobody
    t.run(300, t.a, t.a, 0.0, u);
    EXPECT_EQ(t.wa->closes, 0);
}

TEST(SlowLinks, ASlowLinkWaitsLongerEachTimeItIsReconnectedAgain) {
    TwoLinks t;
    int64_t u = 0;
    t.run(300, t.a, t.b, 74.0, u);
    ASSERT_EQ(t.wb->closes, 1);
    EXPECT_DOUBLE_EQ(t.b.slow_wait, SLOW_LINK_WINDOW_S);
    t.b.connected_at = t.clock();  // made anew, and slow again
    t.run(250, t.a, t.b, 74.0, u);  // 25 s: under its wait of 30 s
    EXPECT_EQ(t.wb->closes, 1);
    t.run(100, t.a, t.b, 74.0, u);
    EXPECT_EQ(t.wb->closes, 2);
    EXPECT_DOUBLE_EQ(t.b.slow_wait, 2 * SLOW_LINK_WINDOW_S);
}

TEST(SlowLinks, OffWithSlowLinkMsZero) {
    TwoLinks t(0.0);
    int64_t u = 0;
    t.run(400, t.a, t.b, 74.0, u);
    EXPECT_EQ(t.wb->closes, 0);
    EXPECT_FALSE(t.feed.behind(t.b));
    EXPECT_EQ(t.feed.links_status(), "links 2/2 first 100/0%");
}

namespace {

Json trade_frame(const std::string& id, const std::string& price, const std::string& size) {
    JsonObject trade {{"side", "SELL"}, {"price", price}, {"size", size}, {"tradeId", id}, {"timestamp", time_ns() / 1000},
                      {"takerAddress", "0xtaker"}};
    return JsonObject {{"type", "channel_data"}, {"channel", "trades"}, {"id", "BTC-USD"}, {"contents", JsonArray {trade}}};
}

}  // namespace

TEST(ArcusTradesFeed, EachFrameOncePassedOnFromTheLinkThatBringsItFirst) {
    std::vector<std::string> got;
    ArcusTradesFeed feed("BTC-USD", "wss://api.arcus.xyz/v1/ws", [&](const Json& msg) { got.push_back(dump_json(msg)); }, {}, {},
                         FeedOptions {.connections = 2, .slow_link_ms = 25.0});
    FakeClock clock;
    feed.clock = clock.fn();
    Link& a = feed.links[0];
    Link& b = feed.links[1];
    a.ws = b.ws = std::make_shared<DummyWs>();
    feed.on_message(J(R"({"type": "subscribed", "channel": "trades", "id": "BTC-USD"})"), 0, &a);
    feed.on_message(trade_frame("t1", "100000", "0.1"), 0, &a);
    clock.advance(0.073);
    feed.on_message(trade_frame("t1", "100000", "0.1"), 0, &b);  // the same trade, 73 ms later
    feed.on_message(trade_frame("t2", "100001", "0.2"), 0, &b);  // b first this time
    feed.on_message(trade_frame("t2", "100001", "0.2"), 0, &a);
    EXPECT_EQ(got.size(), 2u);
    EXPECT_EQ(feed.frame_count, 2);
    EXPECT_EQ(a.wins, 1);
    EXPECT_EQ(b.wins, 1);
    EXPECT_NEAR(*feed.behind(b), 73.0, 1e-6);
    EXPECT_FALSE(feed.carries_data(J(R"({"type": "subscribed", "channel": "trades", "id": "BTC-USD"})")));
    EXPECT_TRUE(feed.carries_data(trade_frame("t3", "1", "1")));
}

// The paper gateway on a server sending each trade on both connections: one fill per trade; ready
// while a connection is up, its orders cancelled ("trades feed lost") once none is.
TEST(PaperGateway, TradesOverTwoLinksFillOnceAndAllDownCancels) {
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
    MarketSpec btc = MarketSpec::from_json(J(R"({"marketId": 1, "marketDisplayName": "BTC-USD", "tickSize": "0.1",
        "stepSize": "0.00000001", "minOrderSize": "0.0001", "minOrderNotional": "5", "maxOrderSize": "10000"})"));
    LogCapture log("naive_mm.paper", logging::INFO);
    PaperGateway gw(btc, "ws://" + server.base() + "/v1/ws");
    gw.trades_options.connections = 2;
    Task task = Task::spawn(gw.run());
    ASSERT_TRUE(run_until(ioc, [&] { return subscribers.size() == 2 && gw.ready; }));
    OrderPtr buy = gw.place("BUY", Decimal("99900"), Decimal("0.001"));
    std::string frame = dump_json(trade_frame("t1", "99900", "0.0004"));
    for (auto& s : subscribers)
        if (auto ws = s.lock()) ws->send_now(frame);
    ASSERT_TRUE(run_until(ioc, [&] { return gw.trades->links[0].wins + gw.trades->links[1].wins == 1 && gw.trades->message_count >= 2; }));
    EXPECT_EQ(buy->filled, Decimal("0.0004"));  // the copy on the other link filled nothing more
    for (auto& s : subscribers)
        if (auto ws = s.lock()) ws->close();
    ASSERT_TRUE(run_until(ioc, [&] { return !gw.ready; }));
    EXPECT_EQ(buy->state, OrderState::CANCELED);
    EXPECT_EQ(buy->reason, "trades feed lost");
    EXPECT_EQ(log.messages(), (std::vector<std::string> {"paper trading on BTC-USD trades", "trades feed lost: every connection is down"}));
    task.cancel();
    run_for(ioc, 0.02);
    server.close();
}
