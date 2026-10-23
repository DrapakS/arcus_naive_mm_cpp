// [queue]: the order placed among the others' levels on its side, within what the markup allows,
// and the paper fills behind the queue at the order's price (paper_queue).
#include <gtest/gtest.h>

#include "config.hpp"
#include "feed_helpers.hpp"
#include "strategy_helpers.hpp"
#include "test_util.hpp"
#include "trading/paper.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

// ETH-USD (tick 0.01), fair bid 2404.42, markup 0.8 bp: the markup's buy is 2404.22, its sell 2404.63.
constexpr double FAIR = 2404.42;
constexpr double MARKUP = 0.00008;

Rig eth(bool snap, double step_share = 0.0, const std::string& direction = "long") {
    StrategyOptions o = options(MARKUP, 0.000002, 0.627);  // tol 0.02 bp: under a tick (0.04 bp)
    o.tol_away = 0.0001;  // 1 bp
    o.direction = direction;
    o.max_position_usd = 100'000;
    o.queue_snap = snap;
    o.queue_step_share = step_share;
    o.queue_wall_x = 3.0;
    Rig r;
    r.gw = std::make_unique<FakeGateway>(eth_spec());
    r.s = std::make_unique<QuoteStrategy>(r.gw.get(), eth_spec(), o);
    return r;
}

MarketView eth_book(double fair, std::vector<Level> bids, std::vector<Level> asks = {{2404.45, 10.0}, {2404.70, 5.0}, {2404.90, 50.0}}) {
    return book(fair, std::move(bids), std::move(asks), "ETH-USD");
}

}  // namespace

TEST(Queue, OffTheOrderStandsAtTheMarkupsPrice) {
    Rig r = eth(false);
    r.s->evaluate(eth_book(FAIR, {{2404.32, 20}, {2404.25, 130}, {2404.10, 500}}));
    EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.22", "0.627")}));
}

TEST(Queue, SnapGoesATickPastTheLevelBehindAndKeepsTheOrderWhileItIsBetween) {
    Rig r = eth(true);
    // nobody between 2404.22 (the markup's) and 2404.10: the same sweeps fill it at 2404.11
    r.s->evaluate(eth_book(FAIR, {{2404.32, 20}, {2404.25, 130}, {2404.10, 500}, {2404.00, 900}}));
    EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.11", "0.627")}));
    r.gw->ack();
    // 2404.10 leaves: the target is now 2404.01, the order stays (within 2404.01..2404.22)
    r.s->evaluate(eth_book(FAIR, {{2404.32, 20}, {2404.25, 130}, {2404.11, 0.627}, {2404.00, 900}}));
    EXPECT_TRUE(actions(*r.gw).empty());
    // the fair rises 0.1 bp: the bound rises, the order still in the band
    r.s->evaluate(eth_book(FAIR + 0.03, {{2404.32, 20}, {2404.25, 130}, {2404.11, 0.627}, {2404.00, 900}}));
    EXPECT_TRUE(actions(*r.gw).empty());
    // the fair falls: the markup's price (2404.02) is under the order by more than tol, it goes
    r.s->evaluate(eth_book(FAIR - 0.2, {{2404.32, 20}, {2404.25, 130}, {2404.11, 0.627}, {2404.00, 900}}));
    auto acts = actions(*r.gw);
    ASSERT_FALSE(acts.empty());
    EXPECT_EQ(acts[0], A("cancel", "BUY", "2404.11", "0.627"));
}

TEST(Queue, SnapLeavesTheOrderAtALevelOrWhenNothingIsSeenBehindIt) {
    {
        Rig r = eth(true);  // a level right at the markup's price: joined
        r.s->evaluate(eth_book(FAIR, {{2404.32, 20}, {2404.22, 130}}));
        EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.22", "0.627")}));
    }
    {
        Rig r = eth(true);  // every level shown is nearer the market: what is behind is not seen
        r.s->evaluate(eth_book(FAIR, {{2404.32, 20}, {2404.25, 130}}));
        EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.22", "0.627")}));
    }
    {
        Rig r = eth(true);  // ahead of all the others (they bid low): a tick over their best
        r.s->evaluate(eth_book(FAIR, {{2404.05, 20}, {2404.00, 130}}));
        EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.06", "0.627")}));
    }
}

TEST(Queue, ASellSnapsUpToATickUnderTheAskBehindIt) {
    Rig r = eth(true, 0.0, "both");
    r.s->evaluate(eth_book(FAIR, {{2404.32, 20}, {2404.10, 500}}, {{2404.45, 10}, {2404.70, 5}, {2404.90, 50}}));
    auto acts = actions(*r.gw);
    EXPECT_NE(std::find(acts.begin(), acts.end(), A("place", "SELL", "2404.69", "0.627")), acts.end());  // the markup's 2404.63
    EXPECT_NE(std::find(acts.begin(), acts.end(), A("place", "BUY", "2404.11", "0.627")), acts.end());
}

TEST(Queue, AWallWithinTheBudgetIsSteppedOver) {
    // budget: a quarter of the markup, 2404.42 x 0.8 bp / 4 = 0.048: one tick over a wall at our price
    {
        Rig r = eth(true, 0.25);
        r.s->evaluate(eth_book(FAIR, {{2404.32, 0.2}, {2404.25, 1.3}, {2404.22, 47.31}, {2404.20, 1.78}}));
        EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.23", "0.627")}));
        r.gw->ack();
        r.s->evaluate(eth_book(FAIR, {{2404.32, 0.2}, {2404.25, 1.3}, {2404.23, 0.627}, {2404.22, 47.31}, {2404.20, 1.78}}));
        EXPECT_TRUE(actions(*r.gw).empty());  // stays
        // the wall leaves: no step to pay for any more, the order is past the markup's price
        r.s->evaluate(eth_book(FAIR, {{2404.32, 0.2}, {2404.25, 1.3}, {2404.23, 0.627}, {2404.20, 1.78}}));
        auto acts = actions(*r.gw);
        ASSERT_FALSE(acts.empty());
        EXPECT_EQ(acts[0], A("cancel", "BUY", "2404.23", "0.627"));
    }
    {
        Rig r = eth(true, 0.25);  // a wall 3 ticks up: over it costs 0.05 > 0.048, not stepped; snapped instead
        r.s->evaluate(eth_book(FAIR, {{2404.32, 0.2}, {2404.26, 47.31}, {2404.20, 1.78}}));
        EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.21", "0.627")}));
    }
    {
        Rig r = eth(false, 0.25);  // a small level (under 3 x 0.627) is no wall
        r.s->evaluate(eth_book(FAIR, {{2404.32, 0.2}, {2404.22, 1.5}}));
        EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.22", "0.627")}));
    }
    {
        Rig r = eth(false, 0.25);  // a wall behind our price: we are ahead of it already
        r.s->evaluate(eth_book(FAIR, {{2404.32, 0.2}, {2404.21, 47.31}}));
        EXPECT_EQ(actions(*r.gw), (std::vector<Action> {A("place", "BUY", "2404.22", "0.627")}));
    }
}

TEST(Queue, ThePlacementLineSaysWhatTheQueueDid) {
    LogCapture log("naive_mm.orders");
    Rig r = eth(true, 0.25);
    r.s->evaluate(eth_book(FAIR, {{2404.32, 0.2}, {2404.22, 47.31}}));
    Rig q = eth(true);
    q.s->evaluate(eth_book(FAIR, {{2404.32, 20}, {2404.10, 500}}));
    std::string text = join(log.messages(), "\n");
    EXPECT_NE(text.find("queue: stepped over 47.31 at 2404.22, 0.04 bp nearer than the markup's 2404.22"), std::string::npos) << text;
    EXPECT_NE(text.find("queue: 0.46 bp back from the markup's 2404.22, a tick past the others' 2404.1"), std::string::npos) << text;
}

TEST(Queue, PaperFillsBehindTheQueueAtTheOrdersPrice) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    OrderBook book_("arcus", "BTC-USD");
    book_.apply_snapshot(std::vector<Level> {{99'900.0, 0.003}, {99'899.9, 1.0}}, std::vector<Level> {{99'900.1, 1.0}}, 1);
    PaperGateway gw(btc_spec(), "ws://unused");
    gw.book = &book_;
    gw.ready = true;
    OrderPtr buy = gw.place("BUY", Decimal("99900"), Decimal("0.001"));
    buy->book_base = 0.003;  // 0.003 stood there as it went out
    auto trades = [](const std::string& price, const std::string& size) {
        return Json(JsonObject {{"type", "channel_data"}, {"channel", "trades"}, {"id", "BTC-USD"},
                                {"contents", JsonArray {JsonObject {{"price", price}, {"size", size}}}}});
    };
    gw.on_message(trades("99900", "0.002"));
    EXPECT_EQ(buy->filled, 0);  // 0.001 still ahead
    book_.apply_update(std::vector<Level> {{99'900.0, 0.0005}}, {}, 2);  // part of those ahead cancelled
    gw.on_message(trades("99900", "0.0008"));
    EXPECT_EQ(buy->filled, Decimal("0.0003"));  // 0.0005 ahead, then ours
    gw.on_message(trades("99899.9", "0.5"));  // through the price: the rest
    EXPECT_EQ(buy->state, OrderState::FILLED);
}

// Snap on the live ETH bot only (23 Oct, the owner's); the step and the paper queue model off live.
TEST(Queue, OnlyTheLiveEthBotSnaps) {
    for (std::string name : {"btc-ml-both", "eth-ml-both", "sol-ml-both", "gld-both", "nvda-both", "spy-both", "qqq-both", "sndk-both"}) {
        Config c = load_config(std::string(NMM_SOURCE_DIR) + "/config." + name + ".toml", {});
        EXPECT_EQ(c.queue.snap, name == "eth-ml-both") << name;
        EXPECT_EQ(c.queue.step_share, 0.0) << name;
        EXPECT_FALSE(c.queue.paper_queue) << name;
    }
    Config plain;
    EXPECT_FALSE(plain.queue.snap);  // off by default
}
