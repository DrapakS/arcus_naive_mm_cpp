#include <gtest/gtest.h>

#include <deque>

#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "feeds/binance.hpp"
#include "test_util.hpp"

using namespace nmm;
using namespace nmm::test;

// designated initializers name only the fields a test sets
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {

Json event(int64_t first, int64_t last, const char* bids = "[]", const char* asks = "[]") {
    return JsonObject {{"e", "depthUpdate"}, {"E", 1}, {"s", "BTCUSDT"}, {"U", first}, {"u", last}, {"b", J(bids)}, {"a", J(asks)}};
}

Json ticker(int64_t update_id, const char* bid, const char* bid_size, const char* ask, const char* ask_size) {
    JsonObject data {{"u", update_id}, {"s", "BTCUSDT"}, {"b", bid}, {"B", bid_size}, {"a", ask}, {"A", ask_size}};
    return JsonObject {{"stream", "btcusdt@bookTicker"}, {"data", data}};
}

Json depth_snapshot(int64_t last_update_id, const char* bids, const char* asks) {
    return JsonObject {{"lastUpdateId", last_update_id}, {"bids", J(bids)}, {"asks", J(asks)}};
}

// Feed whose REST snapshots come from a list instead of the network.
class ScriptedFeed : public BinanceSpotBookFeed {
public:
    ScriptedFeed(OrderBook& book, std::vector<Json> snapshots) : BinanceSpotBookFeed(book), snapshots(snapshots.begin(), snapshots.end()) {
        snapshot_refetch_delay = 0.0;
    }
    awaitable<Json> fetch_snapshot() override {
        ++fetches;
        Json s = snapshots.front();
        snapshots.pop_front();
        co_return s;
    }
    std::deque<Json> snapshots;
    int fetches = 0;
};

struct BinanceScripted : ::testing::Test {
    BinanceScripted() { set_loop(ioc); }
    boost::asio::io_context ioc;
    OrderBook book {"binance", "BTCUSDT"};
};

}  // namespace

TEST_F(BinanceScripted, BuffersUntilSnapshotThenReplaysNewerEvents) {
    ScriptedFeed feed(book, {depth_snapshot(107, R"([["100", "1"]])", R"([["101", "1"]])")});
    feed.on_message(event(101, 105, R"([["100", "5"]])"), 0);
    feed.on_message(event(106, 110, R"([["99", "2"]])"), 0);
    settle(ioc);
    feed.on_message(event(111, 112, "[]", R"([["101", "0"], ["102", "3"]])"), 0);
    EXPECT_EQ(feed.fetches, 1);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 112);
    // event 101..105 is older than the snapshot and must not be applied
    EXPECT_EQ(book.bids(), (Levels {{100.0, 1.0}, {99.0, 2.0}}));
    EXPECT_EQ(book.asks(), (Levels {{102.0, 3.0}}));
}

TEST_F(BinanceScripted, SnapshotOlderThanFirstEventIsRefetched) {
    ScriptedFeed feed(book, {depth_snapshot(150, R"([["1", "1"]])", R"([["2", "1"]])"), depth_snapshot(205, R"([["1", "1"]])", R"([["2", "1"]])")});
    feed.on_message(event(200, 204), 0);
    settle(ioc);
    EXPECT_EQ(feed.fetches, 2);
    EXPECT_FALSE(book.synced);
    feed.on_message(event(205, 210), 0);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 210);
}

TEST_F(BinanceScripted, SnapshotAheadOfStreamWaitsForNewerEvents) {
    ScriptedFeed feed(book, {depth_snapshot(300, R"([["1", "1"]])", R"([["2", "1"]])")});
    feed.on_message(event(290, 295), 0);
    settle(ioc);
    EXPECT_FALSE(book.synced);
    feed.on_message(event(296, 299), 0);
    EXPECT_FALSE(book.synced);
    feed.on_message(event(300, 304), 0);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 304);
    EXPECT_EQ(feed.fetches, 1);
}

TEST_F(BinanceScripted, GapInvalidatesBookAndResyncs) {
    ScriptedFeed feed(book, {depth_snapshot(100, R"([["1", "1"]])", R"([["2", "1"]])"), depth_snapshot(130, R"([["1", "9"]])", R"([["2", "9"]])")});
    feed.on_message(event(99, 101), 0);
    settle(ioc);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 101);
    feed.on_message(event(102, 102), 0);
    feed.on_message(event(102, 102), 0);  // duplicate: ignored
    feed.on_message(event(120, 125), 0);  // 103..119 missing
    EXPECT_FALSE(book.synced);
    EXPECT_EQ(feed.resync_count, 1);
    settle(ioc);
    feed.on_message(event(126, 131), 0);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 131);
    EXPECT_EQ(book.bids(), (Levels {{1.0, 9.0}}));
}

TEST_F(BinanceScripted, BookTickerOverlaysDepthUntilDepthCatchesUp) {
    ScriptedFeed feed(book, {depth_snapshot(100, R"([["100", "1"], ["99", "2"]])", R"([["101", "1"], ["102", "2"]])")});
    feed.on_message(event(99, 101), 0);
    settle(ioc);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 101);
    // 100 bid is gone and a new 100.5 ask arrived, four updates after the depth book
    feed.on_message(ticker(105, "99", "4", "100.5", "3"), 0);
    EXPECT_EQ(book.bids(), (Levels {{99.0, 4.0}}));
    EXPECT_EQ(book.asks(), (Levels {{100.5, 3.0}, {101.0, 1.0}, {102.0, 2.0}}));
    EXPECT_EQ(book.seq, 101);  // depth sync state untouched
    feed.on_message(ticker(104, "98", "1", "103", "1"), 0);  // older than the overlay
    EXPECT_EQ(book.best_bid(), Level(99.0, 4.0));
    feed.on_message(JsonObject {{"stream", "btcusdt@depth@100ms"}, {"data", event(102, 106, R"([["100", "0"], ["99", "4"]])", R"([["100.5", "3"]])")}}, 0);
    EXPECT_FALSE(book.bbo());
    EXPECT_EQ(book.seq, 106);
    EXPECT_EQ(book.bids(), (Levels {{99.0, 4.0}}));
    EXPECT_EQ(book.asks()[0], Level(100.5, 3.0));
    feed.on_message(ticker(106, "1", "1", "2", "1"), 0);  // not newer than the depth book
    EXPECT_FALSE(book.bbo());
    EXPECT_EQ(book.best_bid(), Level(99.0, 4.0));
}

TEST(BinanceFeed, StreamUrl) {
    OrderBook book("binance", "BTCUSDT");
    std::string combined = "wss://stream.binance.com:9443/stream?streams=btcusdt@depth@100ms/btcusdt@bookTicker";
    EXPECT_EQ(BinanceSpotBookFeed(book).url, combined);
    EXPECT_EQ(BinanceSpotBookFeed(book, "BTCUSDT", "wss://stream.binance.com:9443/ws").url, combined);  // earlier config value
    EXPECT_EQ(BinanceSpotBookFeed(book, "BTCUSDT", "wss://data-stream.binance.vision/", "https://api.binance.com", 1000, 1000, false).url,
              "wss://data-stream.binance.vision/stream?streams=btcusdt@depth");
}

namespace {

struct Run {
    Levels bids, asks;
    int64_t resync_count = 0, bbo_count = 0;
    std::vector<int64_t> wins;
};

// Run a feed against the fake until the server is done and the book caught up;
// the levels are taken before the feed stops (a stopped feed invalidates its book).
Run run_feed(boost::asio::io_context& ioc, FakeBinance& server, BookListener on_update = {}, bool book_ticker = true,
             int connections = 1) {
    OrderBook book("binance", "BTCUSDT");
    FeedOptions options;
    options.on_update = std::move(on_update);
    options.connections = connections;
    BinanceSpotBookFeed feed(book, "BTCUSDT", server.ws_url(), server.rest_url(), 1000, 100, book_ticker, options);
    Task task = Task::spawn(feed.run());
    Run run;
    EXPECT_TRUE(run_until(ioc, [&] { return server.done; }, 10));
    EXPECT_TRUE(run_until(ioc, [&] { return book.seq == server.update_id; }));
    run.bids = book.bids();
    run.asks = book.asks();
    run.bbo_count = book.bbo_count;
    task.cancel();
    run_for(ioc, 0.02);
    run.resync_count = feed.resync_count;
    for (auto& link : feed.links) run.wins.push_back(link.wins);
    return run;
}

}  // namespace

TEST(BinanceFeed, MatchesTrueBookOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeBinance server(ioc);
    server.start();
    auto run = run_feed(ioc, server);
    server.close();
    EXPECT_EQ(server.streams, (std::vector<std::string> {"btcusdt@depth@100ms", "btcusdt@bookTicker"}));
    EXPECT_EQ(server.snapshot_requests, 1);
    EXPECT_EQ(run.resync_count, 0);
    EXPECT_GT(run.bbo_count, 0);
    EXPECT_EQ(std::make_pair(run.bids, run.asks), server.book.as_floats());
}

TEST(BinanceFeed, MergedBookIsExactAtEveryUpdate) {
    // After every message the merged book must be exactly the true book at the
    // depth book's update id, with the ticker's true top (at its own, newer
    // update id) replacing every level at or better than it.
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeBinance server(ioc, {.batch = 8});
    server.start();
    struct Seen {
        int64_t depth_seq;
        std::optional<int64_t> bbo_seq;
        Levels bids, asks;
        std::optional<Level> best_bid, best_ask;
    };
    std::vector<Seen> seen;
    run_feed(ioc, server, [&](OrderBook& book) {
        if (!book.synced) return;
        auto bbo = book.bbo();
        seen.push_back({book.seq, bbo ? std::optional(bbo->seq) : std::nullopt, book.bids(), book.asks(), book.best_bid(), book.best_ask()});
    });
    server.close();
    size_t overlaid = std::count_if(seen.begin(), seen.end(), [](const Seen& s) { return s.bbo_seq.has_value(); });
    EXPECT_GT(overlaid, 0u);  // both sources were exercised
    EXPECT_LT(overlaid, seen.size());
    for (const auto& s : seen) {
        auto [true_bids, true_asks] = server.books.at(s.depth_seq);
        if (s.bbo_seq) {
            auto [bid, bid_size, ask, ask_size] = server.tops.at(*s.bbo_seq);
            Levels b {{bid, bid_size}}, a {{ask, ask_size}};
            for (auto& l : true_bids)
                if (l.first < bid) b.push_back(l);
            for (auto& l : true_asks)
                if (l.first > ask) a.push_back(l);
            true_bids = b;
            true_asks = a;
        }
        ASSERT_EQ(s.bids, true_bids) << s.depth_seq;
        ASSERT_EQ(s.asks, true_asks) << s.depth_seq;
        ASSERT_EQ(s.best_bid, s.bids[0]);
        ASSERT_EQ(s.best_ask, s.asks[0]);
        // the top itself is the true top at the newest update id the book knows
        auto [tb, tbs, ta, tas] = server.tops.at(s.bbo_seq.value_or(s.depth_seq));
        ASSERT_EQ(s.best_bid, Level(tb, tbs));
        ASSERT_EQ(s.best_ask, Level(ta, tas));
    }
}

TEST(BinanceFeed, DepthOnlyWithoutBookTicker) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeBinance server(ioc);
    server.start();
    auto run = run_feed(ioc, server, {}, false);
    server.close();
    EXPECT_EQ(server.streams, (std::vector<std::string> {"btcusdt@depth@100ms"}));
    EXPECT_EQ(run.bbo_count, 0);
    EXPECT_EQ(std::make_pair(run.bids, run.asks), server.book.as_floats());
}

TEST(BinanceFeed, RecoversFromDroppedEventOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeBinance server(ioc, {.drop = {30}});
    server.start();
    auto run = run_feed(ioc, server);
    server.close();
    EXPECT_EQ(run.resync_count, 1);
    EXPECT_GE(server.snapshot_requests, 2);
    EXPECT_EQ(std::make_pair(run.bids, run.asks), server.book.as_floats());
}

TEST(BinanceFeed, TwoLinksMergeIntoTheTrueBookOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeBinance server(ioc, {}, {.clients = 2, .delays = {{1, 0.004}}, .drops = {{0, {10, 30}}, {1, {50}}}});
    server.start();
    auto run = run_feed(ioc, server, {}, true, 2);
    server.close();
    EXPECT_EQ(std::make_pair(run.bids, run.asks), server.book.as_floats());
    EXPECT_EQ(run.resync_count, 0);  // each lost batch came on the other link
    for (auto wins : run.wins) EXPECT_GT(wins, 0);
    EXPECT_GT(run.bbo_count, 0);
}
