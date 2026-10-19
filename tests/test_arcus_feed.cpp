#include <gtest/gtest.h>

#include <deque>

#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "feeds/arcus.hpp"
#include "test_util.hpp"

using namespace nmm;
using namespace nmm::test;

// designated initializers name only the fields a test sets
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {

const char* CHANNEL = "l2OrderbookUpdates";

Json snapshot(int64_t seq, const char* bids, const char* asks) {
    return JsonObject {{"bids", J(bids)}, {"asks", J(asks)}, {"lastSequenceId", seq}, {"globalSequenceId", seq * 13},
                       {"timestamp", int64_t {1'780'410'803'123'456}}};
}

Json delta(int64_t seq, const char* bids = "[]", const char* asks = "[]", const char* market = "BTC-USD") {
    JsonObject contents {{"bids", J(bids)}, {"asks", J(asks)}, {"lastSequenceId", seq}, {"globalSequenceId", seq * 13}};
    return JsonObject {{"type", "channel_data"}, {"channel", CHANNEL}, {"id", market}, {"contents", contents}};
}

// Feed whose REST snapshots come from a list and whose sends are recorded.
class ScriptedFeed : public ArcusBookFeed {
public:
    ScriptedFeed(OrderBook& book, std::vector<Json> snapshots = {}, int n_levels = 100, int min_levels = 10, int connections = 1)
        : ArcusBookFeed(book, "BTC-USD", "wss://api.arcus.xyz/v1/ws", "https://api.arcus.xyz", n_levels, min_levels,
                        FeedOptions {.connections = connections}),
          snapshots(snapshots.begin(), snapshots.end()) {
        snapshot_refetch_delay = 0.0;
    }
    void send_json(const Json& payload, Link* link = nullptr) override { sent.push_back(payload); }
    awaitable<Json> fetch_snapshot() override {
        ++fetches;
        Json s = snapshots.front();
        snapshots.pop_front();
        co_return s;
    }
    std::deque<Json> snapshots;
    int fetches = 0;
    std::vector<Json> sent;
};

struct ArcusScripted : ::testing::Test {
    ArcusScripted() { set_loop(ioc); }
    boost::asio::io_context ioc;
    OrderBook book {"arcus", "BTC-USD"};
};

std::pair<Link*, Link*> two_links(ScriptedFeed& feed) {
    auto ws = std::make_shared<DummyWs>();
    feed.links[0].ws = ws;  // both connected
    feed.links[1].ws = ws;
    return {&feed.links[0], &feed.links[1]};
}

}  // namespace

TEST_F(ArcusScripted, SubscribesToDeltasOnly) {
    ScriptedFeed feed(book);
    feed.on_connected(feed.links[0]);
    ASSERT_EQ(feed.sent.size(), 1u);
    EXPECT_EQ(feed.sent[0], J(R"({"type": "subscribe", "channel": "l2OrderbookUpdates", "id": "BTC-USD", "snapshot": false})"));
}

TEST_F(ArcusScripted, ConnectedFrameIsNotAWarning) {
    LogCapture logs("naive_mm.arcus", logging::DEBUG);
    ScriptedFeed feed(book);
    feed.on_message(J(R"({"type": "connected", "connection_id": "1791879545010144954"})"), 0);
    feed.on_message(J(R"({"type": "error", "message": "boom"})"), 0);
    auto warnings = logs.messages_at(logging::WARNING);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("boom"), std::string::npos);
}

TEST_F(ArcusScripted, SeedsFromSnapshotAndReplaysNewerDeltas) {
    ScriptedFeed feed(book, {snapshot(101, R"([["94500", "1.5"]])", R"([["94501", "0.8"]])")});
    feed.on_message(J(R"({"type": "subscribed", "channel": "l2OrderbookUpdates", "id": "BTC-USD", "contents": {}})"), 0);
    feed.on_message(delta(101, R"([["94500", "9"]])"), 0);  // already in the snapshot
    feed.on_message(delta(102, R"([["94500.5", "2"]])"), 0);
    EXPECT_FALSE(book.synced);
    settle(ioc);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 102);
    EXPECT_EQ(feed.fetches, 1);
    EXPECT_FALSE(book.exchange_ts_us);  // deltas carry no timestamp
    feed.on_message(delta(103, "[]", R"([["94501", "0"], ["94502", "1"]])"), 0);
    EXPECT_EQ(book.bids(), (Levels {{94500.5, 2.0}, {94500.0, 1.5}}));
    EXPECT_EQ(book.asks(), (Levels {{94502.0, 1.0}}));
}

TEST_F(ArcusScripted, RefetchesSnapshotOlderThanBufferedDeltas) {
    ScriptedFeed feed(book, {snapshot(150, R"([["1", "1"]])", R"([["2", "1"]])"), snapshot(205, R"([["1", "2"]])", R"([["2", "2"]])")});
    feed.on_message(delta(200), 0);  // 151..199 were never sent to us
    settle(ioc);
    EXPECT_EQ(feed.fetches, 2);
    EXPECT_FALSE(book.synced);  // 205 is ahead: wait for 206
    for (int64_t seq = 201; seq < 207; ++seq) feed.on_message(delta(seq), 0);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 206);
    EXPECT_EQ(book.best_bid(), Level(1.0, 2.0));
}

TEST_F(ArcusScripted, MidStreamGapRebuildsFromSnapshotWithoutResubscribing) {
    ScriptedFeed feed(book, {snapshot(100, R"([["1", "1"]])", R"([["2", "1"]])"), snapshot(110, R"([["1", "2"]])", R"([["2", "2"]])")});
    feed.on_message(delta(100), 0);
    feed.on_message(delta(101), 0);
    settle(ioc);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 101);
    feed.on_message(delta(103), 0);  // 102 missing
    EXPECT_FALSE(book.synced);
    EXPECT_EQ(feed.resync_count, 1);
    settle(ioc);
    feed.on_message(delta(111), 0);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 111);
    EXPECT_EQ(book.best_bid(), Level(1.0, 2.0));
    EXPECT_TRUE(feed.sent.empty());
    EXPECT_EQ(feed.fetches, 2);
}

TEST_F(ArcusScripted, DeltasPastTheSnapshotRangeAreIgnored) {
    const char* bids = R"([["100", "1"], ["99", "1"], ["98", "1"]])";
    const char* asks = R"([["101", "1"], ["102", "1"], ["103", "1"]])";
    ScriptedFeed feed(book, {snapshot(10, bids, asks)}, 3, 2);
    feed.on_message(delta(10), 0);
    settle(ioc);
    // 97 and 104 lie past the known range: levels between are unknown, so skip them
    feed.on_message(delta(11, R"([["97", "5"], ["99.5", "2"]])", R"([["104", "1"]])"), 0);
    EXPECT_EQ(book.bids(), (Levels {{100.0, 1.0}, {99.5, 2.0}, {99.0, 1.0}, {98.0, 1.0}}));
    EXPECT_EQ(book.asks(), (Levels {{101.0, 1.0}, {102.0, 1.0}, {103.0, 1.0}}));
    EXPECT_EQ(book.seq, 11);
}

TEST_F(ArcusScripted, ShortSnapshotSideIsTheWholeSide) {
    ScriptedFeed feed(book, {snapshot(10, R"([["100", "1"]])", R"([["101", "1"], ["102", "1"], ["103", "1"]])")}, 3, 2);
    feed.on_message(delta(10), 0);
    settle(ioc);
    feed.on_message(delta(11, R"([["90", "5"]])", R"([["104", "1"]])"), 0);
    EXPECT_EQ(book.bids(), (Levels {{100.0, 1.0}, {90.0, 5.0}}));  // one bid level means no deeper bids
    EXPECT_EQ(book.asks().back(), Level(103.0, 1.0));
}

TEST_F(ArcusScripted, RefreshesSnapshotWhenKnownLevelsRunOut) {
    const char* bids = R"([["100", "1"], ["99", "1"], ["98", "1"]])";
    const char* asks = R"([["101", "1"], ["102", "1"], ["103", "1"]])";
    ScriptedFeed feed(book, {snapshot(10, bids, asks), snapshot(12, R"([["97", "1"]])", R"([["101", "1"]])")}, 3, 2);
    feed.on_message(delta(10), 0);
    settle(ioc);
    feed.on_message(delta(11, R"([["100", "0"]])"), 0);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(feed.fetches, 1);
    feed.on_message(delta(12, R"([["99", "0"]])"), 0);  // one known bid left
    EXPECT_FALSE(book.synced);
    EXPECT_EQ(feed.refresh_count, 1);
    EXPECT_EQ(feed.resync_count, 0);
    settle(ioc);
    feed.on_message(delta(13), 0);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.best_bid(), Level(97.0, 1.0));
    EXPECT_TRUE(feed.sent.empty());
    EXPECT_EQ(feed.fetches, 2);
}

TEST_F(ArcusScripted, IgnoresOtherMarketsAndChannels) {
    ScriptedFeed feed(book);
    feed.on_message(delta(100, "[]", "[]", "ETH-USD"), 0);
    feed.on_message(J(R"({"type": "channel_data", "channel": "trades", "id": "BTC-USD", "contents": []})"), 0);
    EXPECT_TRUE(feed.buffer().empty());
    EXPECT_EQ(feed.fetches, 0);
}

// ------------------------------------------------------------- several links

TEST_F(ArcusScripted, LinkAheadWaitsForTheLaggingLinkToFillAGap) {
    ScriptedFeed feed(book, {snapshot(100, R"([["1", "1"]])", R"([["2", "1"]])")}, 100, 10, 2);
    auto [a, b] = two_links(feed);
    feed.on_message(delta(101), 0, a);
    feed.on_message(delta(101), 0, b);
    settle(ioc);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 101);
    feed.on_message(delta(103, R"([["1", "3"]])"), 0, a);  // a lost 102 on its way
    EXPECT_EQ(book.seq, 101);  // parked: b is behind and has it
    EXPECT_EQ(feed.resync_count, 0);
    feed.on_message(delta(102, R"([["1", "2"]])"), 0, b);
    EXPECT_EQ(book.seq, 103);
    EXPECT_EQ(book.bids(), (Levels {{1.0, 3.0}}));
    feed.on_message(delta(103, R"([["1", "3"]])"), 0, b);  // b's copy: already applied
    EXPECT_EQ(book.seq, 103);
    EXPECT_EQ(feed.resync_count, 0);
    EXPECT_EQ(a->wins, 1);  // 102 came first on b, 103 on a
    EXPECT_EQ(b->wins, 1);
}

TEST_F(ArcusScripted, GapOnEveryLinkResyncsWithoutWaiting) {
    ScriptedFeed feed(book, {snapshot(100, R"([["1", "1"]])", R"([["2", "1"]])"), snapshot(105, R"([["1", "5"]])", R"([["2", "5"]])")}, 100, 10, 2);
    auto [a, b] = two_links(feed);
    feed.on_message(delta(101), 0, a);
    feed.on_message(delta(101), 0, b);
    settle(ioc);
    feed.on_message(delta(103), 0, a);  // 102 lost everywhere
    EXPECT_EQ(feed.resync_count, 0);  // b might still have it
    feed.on_message(delta(103), 0, b);  // b skips it too: nobody can fill the gap
    EXPECT_EQ(feed.resync_count, 1);
    EXPECT_FALSE(book.synced);
    feed.on_message(delta(106), 0, a);
    settle(ioc);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 106);
}

TEST_F(ArcusScripted, UnfilledGapResyncsAfterTheWait) {
    ScriptedFeed feed(book, {snapshot(100, R"([["1", "1"]])", R"([["2", "1"]])"), snapshot(104, R"([["1", "4"]])", R"([["2", "4"]])")}, 100, 10, 2);
    feed.gap_wait = 0.05;
    auto [a, b] = two_links(feed);
    feed.on_message(delta(101), 0, a);
    feed.on_message(delta(101), 0, b);
    settle(ioc);
    feed.on_message(delta(103), 0, a);  // parked; b never sends anything again
    run_for(ioc, 0.1);
    EXPECT_EQ(feed.resync_count, 1);
    feed.on_message(delta(105), 0, a);
    settle(ioc);
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 105);
}

TEST_F(ArcusScripted, OneLinkDownKeepsTheBook) {
    ScriptedFeed feed(book, {snapshot(100, R"([["1", "1"]])", R"([["2", "1"]])")}, 100, 10, 2);
    auto [a, b] = two_links(feed);
    feed.on_message(delta(101), 0, a);
    settle(ioc);
    a->ws = nullptr;
    feed.on_disconnected(a);
    EXPECT_TRUE(book.synced);  // b is still up
    b->ws = nullptr;
    feed.on_disconnected(b);
    EXPECT_FALSE(book.synced);
}

// ---------------------------------------------------------------- over websocket

namespace {

struct Run {
    Levels bids, asks;
    std::optional<double> floor, ceiling;
    int64_t resync_count = 0;
    std::vector<int64_t> wins;
};

// Run a feed against the fake until the server is done and the book caught up; the
// levels and the known range are taken before the feed stops.
Run run_feed(boost::asio::io_context& ioc, FakeArcus& server, int n_levels = 100, int min_levels = 10, int connections = 1) {
    OrderBook book("arcus", "BTC-USD");
    ArcusBookFeed feed(book, "BTC-USD", server.ws_url(), server.rest_url(), n_levels, min_levels, FeedOptions {.connections = connections});
    Task task = Task::spawn(feed.run());
    Run run;
    EXPECT_TRUE(run_until(ioc, [&] { return server.done; }, 10));
    EXPECT_TRUE(run_until(ioc, [&] { return book.seq == server.seq; }));
    run.bids = book.bids();
    run.asks = book.asks();
    run.floor = feed.bid_floor;
    run.ceiling = feed.ask_ceiling;
    task.cancel();
    run_for(ioc, 0.02);
    run.resync_count = feed.resync_count;
    for (auto& link : feed.links) run.wins.push_back(link.wins);
    return run;
}

std::pair<Levels, Levels> true_levels(FakeArcus& server, std::optional<double> floor, std::optional<double> ceiling) {
    auto [bids, asks] = server.book.as_floats();
    Levels b, a;
    for (auto& l : bids)
        if (!floor || l.first >= *floor) b.push_back(l);
    for (auto& l : asks)
        if (!ceiling || l.first <= *ceiling) a.push_back(l);
    return {b, a};
}

}  // namespace

TEST(ArcusFeed, MatchesTrueBookOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeArcus server(ioc, {.n_events = 400});
    server.start();
    auto run = run_feed(ioc, server);
    server.close();
    EXPECT_EQ(std::make_pair(run.bids, run.asks), true_levels(server, run.floor, run.ceiling));
    EXPECT_EQ(std::make_pair(run.bids, run.asks), server.book.as_floats());
    EXPECT_EQ(run.resync_count, 0);
    EXPECT_GE(server.snapshot_requests, 1);  // refetched while too old
    ASSERT_EQ(server.received.size(), 1u);
    EXPECT_EQ(server.received[0], J(R"({"type": "subscribe", "channel": "l2OrderbookUpdates", "id": "BTC-USD", "snapshot": false})"));
}

TEST(ArcusFeed, RefetchesStaleSnapshotOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeArcus server(ioc, {.n_events = 400, .rest_lags = {60}});  // the first snapshot predates our first delta
    server.start();
    auto run = run_feed(ioc, server);
    server.close();
    EXPECT_GE(server.snapshot_requests, 2);
    EXPECT_EQ(run.resync_count, 0);
    EXPECT_EQ(std::make_pair(run.bids, run.asks), true_levels(server, run.floor, run.ceiling));
}

TEST(ArcusFeed, BookLimitedToSnapshotCoverageOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeArcus server(ioc, {.n_events = 400});  // 10 price levels per side, deltas at any of them
    server.start();
    auto run = run_feed(ioc, server, 5, 3);
    server.close();
    EXPECT_TRUE(run.floor || run.ceiling);
    EXPECT_EQ(std::make_pair(run.bids, run.asks), true_levels(server, run.floor, run.ceiling));
    // Whether the walk leaves the covered range (and the book is refreshed)
    // depends on when the snapshot is taken; the scripted test above covers that.
    EXPECT_EQ(run.resync_count, 0);
}

TEST(ArcusFeed, RecoversFromDroppedDeltaOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeArcus server(ioc, {.n_events = 400, .drop = {200}});
    server.start();
    auto run = run_feed(ioc, server);
    server.close();
    EXPECT_EQ(run.resync_count, 1);
    EXPECT_GE(server.snapshot_requests, 2);
    EXPECT_EQ(server.received.size(), 1u);  // rebuilt from REST, no resubscribe
    EXPECT_EQ(std::make_pair(run.bids, run.asks), true_levels(server, run.floor, run.ceiling));
}

TEST(ArcusFeed, TwoLinksMergeIntoTheTrueBookOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeArcus server(ioc, {.n_events = 400}, {.clients = 2, .delays = {{1, 0.004}}, .drops = {{0, {120, 121, 250}}, {1, {300}}}});
    server.start();
    auto run = run_feed(ioc, server, 100, 10, 2);
    server.close();
    EXPECT_EQ(std::make_pair(run.bids, run.asks), true_levels(server, run.floor, run.ceiling));
    EXPECT_EQ(std::make_pair(run.bids, run.asks), server.book.as_floats());
    EXPECT_EQ(run.resync_count, 0);  // every lost delta came on the other link
    for (auto wins : run.wins) EXPECT_GT(wins, 0);
}

TEST(ArcusFeed, DroppedLinkReconnectsWithoutDisturbingTheBookOverWebsocket) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    FakeArcus server(ioc, {.n_events = 400}, {.clients = 2, .delays = {{1, 0.004}}});
    server.start();
    OrderBook book("arcus", "BTC-USD");
    std::vector<int64_t> unsynced_after_start;
    FeedOptions options {.on_update = [&](OrderBook& b) {
                             if (b.seq && !b.synced) unsynced_after_start.push_back(b.seq);
                         },
                         .connections = 2};
    options.reconnect_min = 0.05;
    ArcusBookFeed feed(book, "BTC-USD", server.ws_url(), server.rest_url(), 100, 10, options);
    Task task = Task::spawn(feed.run());
    ASSERT_TRUE(run_until(ioc, [&] { return book.synced && book.seq > server.seq - 300; }));
    server.kick(0);  // the faster route goes away, then comes back mid-stream
    ASSERT_TRUE(run_until(ioc, [&] { return server.done; }, 10));
    ASSERT_TRUE(run_until(ioc, [&] { return book.seq == server.seq; }));
    EXPECT_EQ(std::make_pair(book.bids(), book.asks()), server.book.as_floats());
    task.cancel();
    run_for(ioc, 0.02);
    server.close();
    EXPECT_EQ(feed.resync_count, 0);
    EXPECT_TRUE(unsynced_after_start.empty());
    EXPECT_EQ(feed.connect_count, 3);
    EXPECT_EQ(server.joined.size(), 3u);
}
