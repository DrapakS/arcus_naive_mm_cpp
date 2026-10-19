#include <gtest/gtest.h>

#include "feed_helpers.hpp"
#include "orderbook.hpp"
#include "util/clock.hpp"
#include "view.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

OrderBook make_book(std::optional<int> max_depth = std::nullopt) {
    OrderBook book("test", "BTC-USD", max_depth);
    book.apply_snapshot(J(R"([["100.0", "1"], ["99.5", "2"], ["99.0", "3"]])"),
                        J(R"([["100.5", "1.5"], ["101.0", "2.5"], ["102.0", "0.5"]])"), 10, 123);
    return book;
}

const Json EMPTY = JsonArray {};

}  // namespace

TEST(OrderBook, SnapshotOrdersLevelsBestFirst) {
    auto book = make_book();
    EXPECT_TRUE(book.synced);
    EXPECT_EQ(book.seq, 10);
    EXPECT_EQ(book.exchange_ts_us, 123);
    EXPECT_EQ(book.bids(), (Levels {{100.0, 1.0}, {99.5, 2.0}, {99.0, 3.0}}));
    EXPECT_EQ(book.asks(), (Levels {{100.5, 1.5}, {101.0, 2.5}, {102.0, 0.5}}));
    EXPECT_EQ(book.best_bid(), Level(100.0, 1.0));
    EXPECT_EQ(book.best_ask(), Level(100.5, 1.5));
    EXPECT_EQ(book.mid(), 100.25);
    EXPECT_EQ(book.bids(2), (Levels {{100.0, 1.0}, {99.5, 2.0}}));
    EXPECT_EQ(book.asks(1), (Levels {{100.5, 1.5}}));
    EXPECT_TRUE(book.bids(0).empty() && book.asks(0).empty());
}

TEST(OrderBook, UpdateSetsAbsoluteSizesAndZeroRemoves) {
    auto book = make_book();
    book.apply_update(J(R"([["100.0", "0"], ["99.75", "4"], ["99.5", "0.25"]])"), J(R"([["100.5", "0.00000000"], ["100.25", "7"]])"), 11);
    EXPECT_EQ(book.seq, 11);
    EXPECT_EQ(book.bids(), (Levels {{99.75, 4.0}, {99.5, 0.25}, {99.0, 3.0}}));
    EXPECT_EQ(book.asks(), (Levels {{100.25, 7.0}, {101.0, 2.5}, {102.0, 0.5}}));
}

TEST(OrderBook, SnapshotReplacesPreviousState) {
    auto book = make_book();
    book.apply_snapshot(J(R"([["90", "1"]])"), J(R"([["91", "1"]])"), 50);
    EXPECT_EQ(book.bids(), (Levels {{90.0, 1.0}}));
    EXPECT_EQ(book.asks(), (Levels {{91.0, 1.0}}));
}

TEST(OrderBook, EqualPricesWithDifferentFormattingAreOneLevel) {
    auto book = make_book();
    book.apply_update(J(R"([["99.50000000", "9"]])"), EMPTY, 11);
    EXPECT_EQ(book.bids(2), (Levels {{100.0, 1.0}, {99.5, 9.0}}));
}

TEST(OrderBook, DictLevelsAreAccepted) {
    OrderBook book("test", "X");
    book.apply_snapshot(J(R"([{"price": "10", "size": "1"}])"), J(R"([{"price": "11", "size": "2"}])"), 1);
    EXPECT_EQ(book.best_bid(), Level(10.0, 1.0));
    EXPECT_EQ(book.best_ask(), Level(11.0, 2.0));
}

TEST(OrderBook, MaxDepthDropsWorstLevels) {
    auto book = make_book(2);
    EXPECT_EQ(book.bids(), (Levels {{100.0, 1.0}, {99.5, 2.0}}));
    EXPECT_EQ(book.asks(), (Levels {{100.5, 1.5}, {101.0, 2.5}}));
    book.apply_update(J(R"([["100.25", "1"]])"), J(R"([["100.75", "1"]])"), 11);
    EXPECT_EQ(book.bids(), (Levels {{100.25, 1.0}, {100.0, 1.0}}));
    EXPECT_EQ(book.asks(), (Levels {{100.5, 1.5}, {100.75, 1.0}}));
}

TEST(OrderBook, InvalidateAndReadiness) {
    auto book = make_book();
    EXPECT_TRUE(book.is_ready() && book.is_ready(1.0));
    book.updated_mono = monotonic() - 10;
    EXPECT_TRUE(book.is_ready());
    EXPECT_FALSE(book.is_ready(1.0));
    book.invalidate();
    EXPECT_FALSE(book.synced);
    EXPECT_EQ(book.depth(), (std::pair<size_t, size_t> {0, 0}));
    EXPECT_FALSE(book.is_ready());
}

TEST(OrderBook, CrossedBookIsNotReady) {
    auto book = make_book();
    book.apply_update(J(R"([["101.5", "1"]])"), EMPTY, 11);
    EXPECT_TRUE(book.is_crossed());
    EXPECT_FALSE(book.is_ready());
}

TEST(OrderBook, TopIsAnImmutableView) {
    auto book = make_book();
    auto top = book.top(2);
    book.apply_update(J(R"([["100.0", "0"]])"), EMPTY, 11);
    EXPECT_EQ(top->bids, (Levels {{100.0, 1.0}, {99.5, 2.0}}));
    EXPECT_EQ(top->asks, (Levels {{100.5, 1.5}, {101.0, 2.5}}));
    EXPECT_EQ(top->mid(), 100.25);
    EXPECT_EQ(top->spread(), 0.5);
    EXPECT_EQ(top->seq, 10);
}

TEST(OrderBook, BboOverlayReplacesStaleTopUntilDepthCatchesUp) {
    auto book = make_book();  // depth at seq 10
    EXPECT_TRUE(book.apply_bbo(99.5, 4.0, 100.25, 2.0, 12));
    EXPECT_EQ(book.seq, 10);  // depth state untouched
    EXPECT_EQ(book.bids(), (Levels {{99.5, 4.0}, {99.0, 3.0}}));  // 100.0 was taken out by update 12
    EXPECT_EQ(book.asks(), (Levels {{100.25, 2.0}, {100.5, 1.5}, {101.0, 2.5}, {102.0, 0.5}}));
    EXPECT_EQ(book.bids(1), (Levels {{99.5, 4.0}}));
    EXPECT_EQ(book.asks(2), (Levels {{100.25, 2.0}, {100.5, 1.5}}));
    EXPECT_TRUE(book.bids(0).empty() && book.asks(0).empty());
    EXPECT_EQ(book.best_bid(), Level(99.5, 4.0));
    EXPECT_EQ(book.mid(), (99.5 + 100.25) / 2);
    auto top = book.top(2);
    EXPECT_EQ(top->seq, 12);
    EXPECT_EQ(top->bids, (Levels {{99.5, 4.0}, {99.0, 3.0}}));
    EXPECT_EQ(top->mid(), book.mid());
    EXPECT_FALSE(book.apply_bbo(1.0, 1.0, 2.0, 1.0, 11));  // older than the overlay
    book.apply_update(J(R"([["100.0", "0"], ["99.5", "4"]])"), J(R"([["100.25", "2"]])"), 12);
    EXPECT_FALSE(book.bbo());
    EXPECT_EQ(book.top(2)->seq, 12);
    EXPECT_EQ(book.bids(), (Levels {{99.5, 4.0}, {99.0, 3.0}}));
    EXPECT_FALSE(book.apply_bbo(1.0, 1.0, 2.0, 1.0, 12));  // not newer than the depth book
}

TEST(OrderBook, BboOverlayHidesLevelsTheMarketMovedThrough) {
    auto book = make_book();
    book.apply_bbo(100.75, 1.0, 101.0, 0.7, 11);  // bid now above the depth's best ask
    EXPECT_EQ(book.bids(), (Levels {{100.75, 1.0}, {100.0, 1.0}, {99.5, 2.0}, {99.0, 3.0}}));
    EXPECT_EQ(book.asks(), (Levels {{101.0, 0.7}, {102.0, 0.5}}));
    EXPECT_FALSE(book.is_crossed());
    EXPECT_TRUE(book.is_ready());
}

TEST(OrderBook, BboOverlaySideWithoutSizeIsIgnored) {
    auto book = make_book();
    book.apply_bbo(0.0, 0.0, 100.25, 2.0, 12);
    EXPECT_EQ(book.bids(1), (Levels {{100.0, 1.0}}));
    EXPECT_EQ(book.best_ask(), Level(100.25, 2.0));
}

TEST(OrderBook, BboOverlayDoesNotRefreshDepthAge) {
    auto book = make_book();
    book.updated_mono = monotonic() - 10;
    book.apply_bbo(99.5, 4.0, 100.25, 2.0, 12);
    EXPECT_TRUE(book.is_ready());
    EXPECT_FALSE(book.is_ready(1.0));
}

TEST(OrderBook, InvalidateDropsBboOverlay) {
    auto book = make_book();
    book.apply_bbo(99.5, 4.0, 100.25, 2.0, 12);
    book.invalidate();
    EXPECT_FALSE(book.bbo());
    EXPECT_FALSE(book.best_bid());
    EXPECT_TRUE(book.bids().empty());
    EXPECT_TRUE(book.apply_bbo(99.5, 4.0, 100.25, 2.0, 13));  // accepted again while resyncing
}

TEST(OrderBook, ReadsAreWorkedOutOncePerWrite) {
    auto book = make_book();
    auto top = book.top(2);
    EXPECT_EQ(book.top(2), top);  // nothing written since: the same view, not built again
    EXPECT_NE(book.top(3), top);
    EXPECT_EQ(book.top(3)->bids, (Levels {{100.0, 1.0}, {99.5, 2.0}, {99.0, 3.0}}));
    book.apply_update(J(R"([["100.2", "0.5"]])"), EMPTY, 11);
    EXPECT_NE(book.top(2), top);
    EXPECT_EQ(book.top(2)->bids, (Levels {{100.2, 0.5}, {100.0, 1.0}}));
    EXPECT_EQ(book.best_bid(), Level(100.2, 0.5));
    EXPECT_EQ(book.mid(), (100.2 + 100.5) / 2);
    top = book.top(2);
    book.apply_bbo(100.3, 1.0, 100.4, 2.0, 12);  // the overlay is a write too
    EXPECT_NE(book.top(2), top);
    EXPECT_EQ(book.top(2)->bids[0], Level(100.3, 1.0));
    EXPECT_EQ(book.best_ask(), Level(100.4, 2.0));
    EXPECT_FALSE(book.apply_bbo(1.0, 1.0, 2.0, 1.0, 12));  // ignored: the views stand
    EXPECT_EQ(book.best_bid(), Level(100.3, 1.0));
    book.apply_snapshot(J(R"([["90", "1"]])"), J(R"([["91", "1"]])"), 20);
    EXPECT_EQ(book.best_bid(), Level(90.0, 1.0));
    EXPECT_EQ(book.top(1)->asks, (Levels {{91.0, 1.0}}));
    EXPECT_FALSE(book.bbo());
    book.invalidate();
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.mid());
    EXPECT_FALSE(book.is_ready());
    EXPECT_TRUE(book.top(2)->bids.empty() && book.top(2)->asks.empty());
}

TEST(MarketView, BasisInBp) {
    MarketView view;
    EXPECT_FALSE(view.basis_bps());
    OrderBook ref("binance", "BTCUSDT"), venue("arcus", "BTC-USD");
    ref.apply_snapshot(J(R"([["100", "1"]])"), J(R"([["100.2", "1"]])"), 1);
    venue.apply_snapshot(J(R"([["99.9", "1"]])"), J(R"([["100.1", "1"]])"), 1);
    view.reference = ref.top(1);
    view.venue = venue.top(1);
    EXPECT_NEAR(*view.basis_bps(), (100.0 / 100.1 - 1.0) * 1e4, 1e-9);
}
