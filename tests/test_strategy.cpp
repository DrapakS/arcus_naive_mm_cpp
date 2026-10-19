// Ported from tests/test_strategy.py, and the strategy tests of test_guard.py.
#include <gtest/gtest.h>

#include "feed_helpers.hpp"
#include "strategy_helpers.hpp"
#include "test_util.hpp"
#include "trading/guard.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

using Actions = std::vector<Action>;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

Rig short_rig(double markup = 0.0001, double close_markup_cut = 0.0, double tol_away = 0.0) {
    StrategyOptions o = options(markup, 0.00001, 0.001);
    o.direction = "short";
    o.close_markup_cut = close_markup_cut;
    o.tol_away = tol_away;
    Rig r;
    r.gw = std::make_unique<FakeGateway>();
    r.s = std::make_unique<QuoteStrategy>(r.gw.get(), btc_spec(), o);
    return r;
}

Rig lopsided_strategy(int levels = 5, const std::string& sides = "both") {
    Rig r = make(0.0001, 0.0002);
    r.s->imbalance = Imbalance {levels, 5.0, 5.0, 0.0, sides};
    return r;
}

// A guard pausing `side`: after a drop for BUY, after a rise for SELL.
std::unique_ptr<PriceGuard> tripped(const std::string& side = "BUY", double pause_s = 10.0) {
    FakeClock clock;
    auto g = std::make_unique<PriceGuard>(1.0, 0.0001, pause_s, clock.fn());
    g->observe(100'000.0);
    clock.advance(1.0);
    g->observe(side == "BUY" ? 99'000.0 : 101'000.0);
    return g;
}

MarketView eth_view(double fair = 2500.0) { return book(fair, {{2499.99, 10.0}}, {{2500.0, 10.0}}, "ETH-USD"); }

Rig eth(double order_size = 0.017) {
    Rig r;
    r.gw = std::make_unique<FakeGateway>(eth_spec());
    r.s = std::make_unique<QuoteStrategy>(r.gw.get(), eth_spec(), options(0.0001, 0.0002, order_size));
    return r;
}

}  // namespace

TEST(Strategy, BuyPriceIsMarkedDownFairButNeverCrossesTheAsk) {
    auto [gw, s] = make(0.001);
    EXPECT_EQ(s->target(view(100'000.0, 99'990.0, 100'050.3), 0)->price, D("99900.0"));  // fair * (1 - markup)
    EXPECT_EQ(s->target(view(100'000.04, 99'990.0, 100'050.3), 0)->price, D("99900.0"));  // rounded down
    auto r = make(0.0001);
    EXPECT_EQ(r.s->target(view(100'000.0, 99'990.0, 99'980.0), 0)->price, D("99979.9"));  // ask - tick
}

TEST(Strategy, SellPriceIsMarkedUpFairPlusTickButNeverCrossesTheBid) {
    auto [gw, s] = make(0.001);
    auto q = s->target(view(100'000.0, 99'999.0, 99'999.1), D("0.001"));
    EXPECT_EQ(q->side, "SELL");
    EXPECT_EQ(q->size, D("0.001"));
    EXPECT_EQ(q->price, D("100100.2"));  // (100000 + 0.1) * 1.001 = 100100.1001, rounded up
    q = s->target(view(100'000.0, 100'200.0, 100'200.1), D("0.001"));
    EXPECT_EQ(q->price, D("100200.1"));  // bid + tick
}

TEST(Strategy, BuyThenRequoteWhenTargetMovesBeyondTol) {
    auto [gw, s] = make(0.001, 0.0002);
    gw->ready = false;
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});  // gateway not ready
    gw->ready = true;
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.001")}));
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});  // waiting for the exchange
    gw->ack();
    s->evaluate(view(100'015.0));  // target 99914.9: 1.49 bp away, within tol
    EXPECT_EQ(actions(*gw), Actions {});
    s->evaluate(view(100'030.0));  // target 99929.9: 2.99 bp away
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));
    s->evaluate(view(100'030.0));
    EXPECT_EQ(actions(*gw), Actions {});  // cancel in flight
    gw->confirm_cancel();
    s->evaluate(view(100'030.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99929.9", "0.001")}));
}

TEST(Strategy, PartialBuyFillSwitchesToSellingThePosition) {
    auto [gw, s] = make();
    s->evaluate(view(100'000.0));
    gw->ack();
    gw->fill("0.0004");
    EXPECT_EQ(gw->position(), D("0.0004"));
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw).back(), A("cancel", "BUY", "99900.0", "0.001"));
    gw->confirm_cancel();
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "SELL", "100100.2", "0.0004")}));
}

TEST(Strategy, DustFillBelowMinOrderSizeKeepsBuying) {
    auto [gw, s] = make();
    s->evaluate(view(100'000.0));
    gw->ack();
    gw->fill("0.00005");  // cannot be sold: under minOrderSize 0.0001
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.001")}));  // only the original place
    ASSERT_TRUE(gw->order());
    EXPECT_EQ(gw->order()->state, OrderState::OPEN);
}

TEST(Strategy, SellKeepsWorkingUntilFlatThenBuysAgain) {
    auto [gw, s] = make();
    s->evaluate(view(100'000.0));
    gw->ack();
    gw->fill("0.001");
    actions(*gw);
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "SELL", "100100.2", "0.001")}));
    gw->ack();
    gw->fill("0.00095");  // 0.00005 left: below the minimum, still ours to sell
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});
    gw->fill("0.00005");
    EXPECT_EQ(gw->position(), 0);
    EXPECT_FALSE(gw->order());
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.001")}));
}

TEST(Strategy, NoPriceCancelsAndPlacesNothing) {
    auto [gw, s] = make();
    s->evaluate(view(100'000.0));
    gw->ack();
    actions(*gw);
    s->evaluate(view(std::nullopt));
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));
    gw->confirm_cancel();
    s->evaluate(view(std::nullopt));
    EXPECT_EQ(actions(*gw), Actions {});
}

TEST(Strategy, APositionAgainstTheDirectionIsTakenBackToZeroFirst) {
    auto [gw, s] = make(0.0001);
    gw->set_position(D("-0.001"));  // a short, left by a run in direction "short"
    s->evaluate(view(100'000.0, 99'900.0, 100'100.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));  // its size, on the opening side
    gw->set_position(D("-0.00005"));  // too small to be an order alone
    EXPECT_EQ(s->target(view(100'000.0, 99'900.0, 100'100.0), gw->position())->size, D("0.00105"));
}

TEST(Strategy, DirectionShortSellsFirstAndBuysBackAtTheClosingMarkup) {
    MarketView wide = view(100'000.0, 99'900.0, 100'100.0);
    {
        auto [gw, s] = short_rig(0.0001, 0.3);
        EXPECT_EQ(s->open_side, "SELL");
        EXPECT_EQ(s->close_side, "BUY");
        s->evaluate(wide);
        EXPECT_EQ(actions(*gw), (Actions {A("place", "SELL", "100010.2", "0.001")}));  // (fair + tick) * (1 + markup)
    }
    {
        auto [gw, s] = short_rig(0.0001, 0.3);
        gw->set_position(D("-0.001"));
        s->evaluate(wide);
        EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99993.0", "0.001")}));  // fair * (1 - 0.7 * markup): the whole short
    }
    auto [gw, s] = short_rig();
    gw->set_position(D("-0.00005"));  // a crumb of a short, no order working: keep selling until it is an order
    EXPECT_EQ(s->side(gw->position(), &wide), "SELL");
    gw->set_position(D("0.001"));  // a long left by a run in direction "long": sold first
    auto quote = s->target(wide, gw->position());
    EXPECT_EQ(quote->side, "SELL");
    EXPECT_EQ(quote->size, D("0.001"));
}

TEST(Strategy, DirectionShortMirrorsCloseTolAwayAndTheImbalance) {
    {
        auto [gw, s] = short_rig(0.0001, 0.0, 0.0001);
        EXPECT_TRUE(s->needs_move("BUY", D("99900.0"), D("99905.0")));  // the closing buy left under a rising target: tol
        EXPECT_FALSE(s->needs_move("SELL", D("100100.0"), D("100095.0")));  // the opening sell left deeper: tol_away
    }
    auto [gw, s] = short_rig();
    s->imbalance = Imbalance {5, 5.0, 5.0, 0.0, "open"};  // sides "open": the sell here
    auto thin_asks = book(100'000.0, {{99'990.0, 6.0}}, {{99'990.1, 1.0}});
    auto thin_bids = book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 6.0}});
    EXPECT_EQ(s->price("SELL", thin_asks), D("100050.2"));  // x5
    EXPECT_EQ(s->price("BUY", thin_bids), D("99990.0"));  // the closing buy keeps its markup
}

TEST(Strategy, DirectionMustBeLongShortOrBoth) {
    FakeGateway gw;
    StrategyOptions o = options(0.0001, 0.00001, 0.001);
    o.direction = "sideways";
    try {
        QuoteStrategy(&gw, btc_spec(), o);
        ADD_FAILURE();
    } catch (const std::invalid_argument& err) {
        EXPECT_TRUE(contains(err.what(), "direction"));
    }
    o.direction = "both";
    o.max_position_usd = 0;
    try {
        QuoteStrategy(&gw, btc_spec(), o);
        ADD_FAILURE();
    } catch (const std::invalid_argument& err) {
        EXPECT_TRUE(contains(err.what(), "max_position_usd"));
    }
}

TEST(Strategy, RejectsUnusableOrderSize) {
    for (double size : {0.00001, 0.000100001}) {
        try {
            make(0.001, 0.0002, size);
            ADD_FAILURE() << size;
        } catch (const std::invalid_argument& err) {
            EXPECT_TRUE(contains(err.what(), "order_size"));
        }
    }
}

TEST(Strategy, MoveRightAfterPlacingCancelsWithoutWaitingForTheAck) {
    auto [gw, s] = make(0.001, 0.0002);
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.001")}));
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});  // unacknowledged and still on target: leave it
    s->evaluate(view(100'030.0));  // target moved before the ack
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));
    s->evaluate(view(100'030.0));
    EXPECT_EQ(actions(*gw), Actions {});  // the cancel is in flight; nothing is placed meanwhile
}

TEST(Strategy, CheckCancelOnlyEverCancels) {
    auto [gw, s] = make(0.001, 0.0002);
    s->check_cancel(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});  // no order: nothing to cancel, and it never places
    s->evaluate(view(100'000.0));
    actions(*gw);
    s->check_cancel(view(100'030.0));  // before the ack, too
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));
    s->check_cancel(view(100'060.0));
    EXPECT_EQ(actions(*gw), Actions {});  // already being cancelled
    gw->confirm_cancel();
    s->evaluate(view(100'000.0));
    gw->ack();
    actions(*gw);
    s->check_cancel(view(100'010.0));
    EXPECT_EQ(actions(*gw), Actions {});  // within tol
    s->check_cancel(view(std::nullopt));
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));  // no price
}

TEST(Strategy, NeverLeadJoinsTheBestPriceButNeverImprovesIt) {
    auto [gw, s] = make(0.000005, 0.000005);
    auto wide = book(100'000.0, {{99'990.0, 1.0}}, {{99'990.5, 1.0}});  // 5-tick spread
    EXPECT_EQ(s->price("BUY", wide), D("99990.4"));  // ask - tick: alone above everyone's bids
    auto cheap = book(99'970.0, {{99'979.5, 1.0}}, {{99'980.5, 1.0}});
    EXPECT_EQ(s->price("SELL", cheap), D("99979.6"));  // bid + tick: alone below everyone's asks
    s->never_lead = true;
    EXPECT_EQ(s->price("BUY", wide), D("99990.0"));  // joins the best bid
    EXPECT_EQ(s->price("SELL", cheap), D("99980.5"));  // joins the best ask
}

TEST(Strategy, NeverLeadPullsTheOrderWhenEveryoneElseLeavesItsLevel) {
    auto [gw, s] = make(0.000005, 0.01);  // tol 1%: only the book can pull it
    s->never_lead = true;
    s->evaluate(book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));  // joins the best bid
    gw->ack();
    s->check_cancel(book(100'000.0, {{99'990.0, 1.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    EXPECT_EQ(actions(*gw), Actions {});  // one of many at the best bid
    s->check_cancel(book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99990.0", "0.001")}));  // left alone on top
    EXPECT_EQ(gw->why, "the book came to it: others' best bid 99989.9, 99990 when placed");
}

TEST(Strategy, ALevelOurOrderJustLeftIsStillOursForAMoment) {
    // Run 13: the orders channel said CANCELED, the book still showed our order for a moment, and the
    // order placed 0-1 ms later at the same price took it for someone else's.
    LogCapture log("naive_mm.orders");
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    FakeClock clock;
    s->clock = clock.fn();
    s->evaluate(book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    gw->ack();
    s->check_cancel(book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    gw->confirm_cancel();  // everyone else left; ours is cancelled, but the book has not dropped it yet
    actions(*gw);
    auto stale = book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}});
    s->evaluate(stale);  // the pause after the book came to it runs from here
    EXPECT_EQ(actions(*gw), Actions {});
    clock.advance(0.031);
    EXPECT_EQ(gw->recently_cancelled("BUY"), (std::optional<std::pair<double, double>>(std::pair {99'990.0, 0.001})));
    EXPECT_FALSE(gw->recently_cancelled("SELL"));
    s->evaluate(stale);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99989.9", "0.001")}));  // joins the others, not itself
    EXPECT_TRUE(contains(log.messages().back(), "queue: at others' best, joins 2, 0 ahead in the top 1")) << log.messages().back();
    gw->confirm_cancel();
    std::get<2>(gw->gone()["BUY"]) = monotonic() - STALE_LEVEL_S;  // later: a level there is someone else's
    s->evaluate(stale);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));
}

TEST(Strategy, TheWindowEndsOnceTheBookShowsOurLeftoverGone) {
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    s->evaluate(book(100'000.0, {{99'990.0, 0.0012}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));  // joins 0.0002 of someone else's
    gw->ack();
    s->check_cancel(book(100'000.0, {{99'990.0, 0.0012}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    gw->cancel(gw->order(), "test");
    gw->confirm_cancel();
    actions(*gw);
    s->evaluate(book(100'000.0, {{99'990.0, 0.0012}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));  // leftover shown
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));  // joins the foreign 0.0002 again
    s->check_cancel(book(100'000.0, {{99'990.0, 0.0002}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    EXPECT_EQ(actions(*gw), Actions {});
    EXPECT_FALSE(gw->recently_cancelled("BUY"));  // the leftover left: not the book coming
}

TEST(Strategy, NeverLeadPullsTheOrderWhenTheLevelsAheadOfItVanish) {
    auto [gw, s] = make(0.0001, 0.01);
    s->never_lead = true;
    s->evaluate(book(100'000.0, {{99'995.0, 1.0}}, {{99'995.1, 1.0}}));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));  // 5 dollars behind the best bid
    gw->ack();
    s->check_cancel(book(100'000.0, {{99'996.0, 1.0}, {99'990.0, 0.001}}, {{99'996.1, 1.0}}));
    EXPECT_EQ(actions(*gw), Actions {});  // the market went away from it: tol's business
    s->check_cancel(book(100'000.0, {{99'993.0, 1.0}, {99'990.0, 0.001}}, {{99'993.1, 1.0}}));
    EXPECT_EQ(actions(*gw), Actions {});  // coming closer, still levels ahead of it
    s->check_cancel(book(100'000.0, {{99'990.0, 3.001}}, {{99'990.1, 1.0}}));
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99990.0", "0.001")}));  // the touch came down to it
}

TEST(Strategy, NeverLeadSellSideIsTheMirrorImage) {
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    gw->add_position(D("0.001"));  // long: it sells
    s->evaluate(book(99'979.6, {{99'980.0, 1.0}}, {{99'980.1, 1.0}, {99'980.2, 2.0}}));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "SELL", "99980.2", "0.001")}));  // behind the best ask
    gw->ack();
    s->check_cancel(book(99'979.6, {{99'979.4, 1.0}}, {{99'979.5, 1.0}, {99'980.2, 0.001}}));
    EXPECT_EQ(actions(*gw), Actions {});  // the market went away from it (down)
    s->check_cancel(book(99'979.6, {{99'980.1, 1.0}}, {{99'980.2, 2.001}, {99'980.3, 1.0}}));
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "SELL", "99980.2", "0.001")}));  // the asks below it were lifted
}

TEST(Strategy, WithoutNeverLeadTheBookDoesNotPullTheOrder) {
    auto [gw, s] = make(0.000005, 0.01);
    s->evaluate(book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    gw->ack();
    actions(*gw);
    s->check_cancel(book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    EXPECT_EQ(actions(*gw), Actions {});
}

TEST(Strategy, LopsidedBookWidensTheQuoteOnItsThinSide) {
    auto [gw, s] = lopsided_strategy();
    auto even = book(100'000.0, {{99'990.0, 4.0}}, {{99'990.1, 1.0}});  // 4x: not lopsided
    EXPECT_FALSE(s->thin_side(even));
    EXPECT_EQ(s->price("BUY", even), D("99990.0"));
    EXPECT_EQ(s->price("SELL", even), D("100010.2"));
    auto thin_asks = book(100'000.0, {{99'990.0, 6.0}}, {{99'990.1, 1.0}});
    EXPECT_EQ(s->thin_side(thin_asks), (std::optional<std::pair<std::string, double>>(std::pair {std::string("SELL"), 6.0})));
    EXPECT_EQ(s->price("SELL", thin_asks), D("100050.2"));  // (fair + tick) * (1 + 5 * markup)
    EXPECT_EQ(s->price("BUY", thin_asks), D("99990.0"));  // unchanged
    auto thin_bids = book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 6.0}});
    EXPECT_EQ(s->thin_side(thin_bids), (std::optional<std::pair<std::string, double>>(std::pair {std::string("BUY"), 6.0})));
    EXPECT_EQ(s->price("BUY", thin_bids), D("99950.0"));  // fair * (1 - 5 * markup)
    EXPECT_EQ(s->price("SELL", thin_bids), D("100010.2"));
}

TEST(Strategy, ByDefaultALopsidedBookWidensTheOpeningBuyOnly) {
    // Run 17: x5 on the closing sell put it 25 bp over the market a quarter of the time.
    LogCapture log("naive_mm.orders");
    auto [gw, s] = lopsided_strategy(5, "open");
    auto thin_asks = book(100'000.0, {{99'990.0, 6.0}}, {{99'990.1, 1.0}});
    auto thin_bids = book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 6.0}});
    EXPECT_EQ(s->thin_side(thin_asks)->first, "SELL");  // still found: the hold and the logs need it
    EXPECT_EQ(s->price("SELL", thin_asks), D("100010.2"));  // the normal markup
    EXPECT_EQ(s->price("BUY", thin_bids), D("99950.0"));  // x5
    gw->add_position(D("0.001"));  // long: it sells into thin asks
    s->evaluate(thin_asks);
    ASSERT_EQ(gw->sent.size(), 1u);
    EXPECT_EQ(gw->sent[0].second->price, D("100010.2"));
    EXPECT_FALSE(contains(log.messages().back(), "bids 6.0x asks"));
    gw->sent.clear();
    gw->ack();
    auto even = book(100'000.0, {{99'990.0, 4.0}}, {{99'990.1, 1.0}});
    s->check_cancel(even);  // the book evened out: not an order placed under the thin-side markup
    EXPECT_EQ(actions(*gw), Actions {});
}

TEST(Strategy, ImbalanceCountsTheFirstLevelsOnlyAndNotOurOrder) {
    {
        auto [gw, s] = lopsided_strategy(2);
        auto deep = book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 1.0}, {99'989.8, 100.0}}, {{99'990.1, 1.0}});
        EXPECT_FALSE(s->thin_side(deep));  // the 100 on the third level is not counted
    }
    auto [gw, s] = lopsided_strategy();
    s->evaluate(book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 1.0}}));
    gw->ack();  // our 0.001 buy at 99990
    EXPECT_FALSE(s->thin_side(book(100'000.0, {{99'990.0, 5.001}}, {{99'990.1, 1.0}})));  // 5.0 without us
    EXPECT_EQ(s->thin_side(book(100'000.0, {{99'990.0, 5.002}}, {{99'990.1, 1.0}}))->first, "SELL");
}

TEST(Strategy, ThinSideHoldsThroughAFlickerAndLetsGoAfterHold) {
    auto [gw, s] = lopsided_strategy();
    s->imbalance = Imbalance {5, 5.0, 5.0, 0.3, "both"};
    FakeClock clock;
    s->clock = clock.fn();
    gw->add_position(D("0.001"));  // long: it sells, and the asks are the thin side below
    auto thin_asks = book(100'000.0, {{99'990.0, 6.0}}, {{99'990.1, 1.0}});
    auto even = book(100'000.0, {{99'990.0, 4.0}}, {{99'990.1, 1.0}});
    s->evaluate(thin_asks);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "SELL", "100050.2", "0.001")}));  // x5 markup
    EXPECT_FALSE(s->thin_held);  // lopsided now, not held
    gw->ack();
    clock.advance(0.03);
    EXPECT_EQ(s->thin_side(even), (std::optional<std::pair<std::string, double>>(std::pair {std::string("SELL"), 6.0})));  // still held
    EXPECT_TRUE(s->thin_held);
    EXPECT_EQ(lopsided({"SELL", 6.0}, s->thin_held), "held: bids were 6.0x asks");  // the logs say so
    s->check_cancel(even);
    EXPECT_EQ(actions(*gw), Actions {});  // the flicker does not move the order
    clock.advance(0.28);  // 310 ms since the book was last lopsided
    EXPECT_FALSE(s->thin_side(even));
    s->check_cancel(even);
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "SELL", "100050.2", "0.001")}));  // now the normal markup applies
}

TEST(Strategy, TolAwayLetsAnOrderSinkALittleButNotDriftTowardTheMarket) {
    auto [gw, s] = make(0.001, 0.0002);
    s->tol_away = D("0.001");
    s->evaluate(view(100'000.0));
    gw->ack();
    actions(*gw);  // buy at 99900
    s->check_cancel(view(100'050.0));  // target 99949.9: 5 bp above it, the order sits deeper
    EXPECT_EQ(actions(*gw), Actions {});  // within tol_away
    s->check_cancel(view(100'120.0, 100'100.0, 100'200.0));  // target 100019.8, 12 bp: past tol_away too
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));
    gw->confirm_cancel();
    s->evaluate(view(100'000.0));
    gw->ack();
    actions(*gw);
    s->check_cancel(view(99'970.0));  // target 99870.0: 3 bp below the order, toward the market
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));  // tol applies
}

TEST(Strategy, CloseTolAwayForASellIsTheMirrorImage) {
    auto [gw, s] = make(0.001, 0.0002);
    s->close_tol_away = D("0.001");
    Decimal price("100100.2");
    EXPECT_FALSE(s->needs_move("SELL", price, D("100050.0")));  // target 5 bp below: the sell sits deeper
    EXPECT_TRUE(s->needs_move("SELL", price, D("100130.2")));  // target 3 bp above: toward the market
}

TEST(Strategy, ByDefaultTheClosingSellFollowsAFallingTargetAtTol) {
    // Run 19: with tol_away 1-1.5 bp the exit sell sat behind its falling target 56-66% of the time.
    FakeGateway gw;
    StrategyOptions o = options(0.0001, 0.00001, 0.001);
    o.tol_away = 0.0001;
    QuoteStrategy s(&gw, btc_spec(), o);
    EXPECT_EQ(s.tol_away, D("0.0001"));
    EXPECT_EQ(s.close_tol_away, D("0.00001"));
    gw.add_position(D("0.001"));  // long: it sells
    s.evaluate(view(100'000.0, 99'990.0, 100'030.0));
    gw.ack();
    ASSERT_EQ(gw.sent.size(), 1u);
    auto order = gw.sent[0].second;
    gw.sent.clear();
    EXPECT_EQ(order->side, "SELL");
    EXPECT_EQ(order->price, D("100010.2"));
    s.check_cancel(view(99'995.0, 99'990.0, 100'030.0));  // target 100005.2: 0.5 bp under the sell
    EXPECT_EQ(actions(gw), (Actions {A("cancel", "SELL", "100010.2", "0.001")}));  // past tol, not tol_away
    EXPECT_FALSE(s.needs_move("BUY", D("99900.0"), D("99905.0")));  // a buy left 0.5 bp under a rising target keeps tol_away
    EXPECT_TRUE(s.needs_move("BUY", D("99900.0"), D("99895.0")));  // and moves at tol toward the market
}

TEST(Strategy, ASmallForeignLevelAtOurPriceStaysForeignUntilOurOrderIsInTheBook) {
    // Night run 11: a foreign bid of 0.0005 at our price counted as 'others' with no order of ours and
    // as 'us' a millisecond later: the bot cancelled and re-placed at the same price in a loop.
    auto [gw, s] = make(0.000005, 0.000005);
    s->never_lead = true;
    auto thin = book(100'000.0, {{99'990.0, 0.0005}, {99'985.0, 1.0}}, {{99'990.1, 1.0}});
    s->evaluate(thin);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));  // joins the foreign 0.0005
    EXPECT_EQ(s->best_of_others(thin, "BUY"), 99'990.0);  // not in the book yet: that level is someone else's
    s->check_cancel(thin);
    EXPECT_EQ(actions(*gw), Actions {});  // no ghost loop
    gw->ack();  // now it rests: the level shows ours on top of the foreign 0.0005
    auto both_ = book(100'000.0, {{99'990.0, 0.0015}, {99'985.0, 1.0}}, {{99'990.1, 1.0}});
    EXPECT_EQ(s->best_of_others(both_, "BUY"), 99'990.0);
    s->check_cancel(both_);
    EXPECT_EQ(actions(*gw), Actions {});
    auto alone = book(100'000.0, {{99'990.0, 0.001}, {99'985.0, 1.0}}, {{99'990.1, 1.0}});
    s->check_cancel(alone);  // the foreign 0.0005 left: we are alone on top
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99990.0", "0.001")}));
}

TEST(Strategy, AnAcknowledgedOrderCountsAsInTheBookOnceTheOrdersChannelIsLate) {
    auto [gw, s] = make(0.000005, 0.000005);
    s->evaluate(book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 1.0}}));
    auto order = gw->order();
    EXPECT_FALSE(s->own_in_book("BUY"));  // pending, not acknowledged
    order->order_id = "abc";
    order->acked_at = monotonic();  // the placeOrder reply came
    EXPECT_FALSE(s->own_in_book("BUY"));  // just now: the book may not show it yet
    order->placed_at = monotonic() - 1.0;  // a slow reply: counted from the reply, not from placing
    EXPECT_FALSE(s->own_in_book("BUY"));
    order->acked_at = monotonic() - ACK_IN_BOOK_S - 0.01;
    EXPECT_EQ(s->own_in_book("BUY"), (std::optional<std::pair<double, double>>(std::pair {99'990.0, 0.001})));  // by now it is there
    gw->orders_in_public_book = false;  // paper orders never are
    EXPECT_FALSE(s->own_in_book("BUY"));
}

TEST(Strategy, QueueNoteSaysWhereANewOrderJoinsTheOthers) {
    std::vector<Level> bids {{100.0, 1.0}, {99.9, 2.0}, {99.8, 0.5}};
    EXPECT_EQ(queue_note(bids, "BUY", 99.9, 0.1), "queue: 1 tick behind others' best, joins 2, 1 ahead in the top 3");
    EXPECT_EQ(queue_note(bids, "BUY", 100.0, 0.1), "queue: at others' best, joins 1, 0 ahead in the top 3");
    EXPECT_EQ(queue_note(bids, "BUY", 100.1, 0.1), "queue: 1 tick ahead of others' best, a new level, 0 ahead in the top 3");
    EXPECT_EQ(queue_note(bids, "BUY", 99.5, 0.1), "queue: 5 ticks behind others' best, a new level, over 3.5 ahead (deeper than the top 3)");
    std::vector<Level> asks {{100.1, 1.0}, {100.2, 3.0}};
    EXPECT_EQ(queue_note(asks, "SELL", 100.2, 0.1), "queue: 1 tick behind others' best, joins 3, 1 ahead in the top 2");
    EXPECT_EQ(queue_note({}, "SELL", 100.2, 0.1), "queue: the side is empty");
}

TEST(Strategy, EveryPlacementSaysWhereItJoinsTheQueue) {
    LogCapture log("naive_mm.orders");
    auto [gw, s] = make();
    s->evaluate(view(100'000.0));  // buy at 99900, 900 ticks under the only bid
    std::vector<std::string> placed;
    for (const auto& m : log.messages())
        if (m.starts_with(">>> PLACE")) placed.push_back(m);
    EXPECT_EQ(placed, (std::vector<std::string> {
                          ">>> PLACE BUY 0.001 @ 99900 (fair bid 100000.00, Arcus 99990.0/99990.1; "
                          "queue: 900 ticks behind others' best, a new level, over 1 ahead (deeper than the top 1))"}));
}

TEST(Strategy, ALongWorthLessThanTheMinimumNotionalKeepsBuyingUntilItCanBeSold) {
    auto [gw, s] = eth();
    s->evaluate(eth_view());
    gw->ack();
    gw->fill("0.0015");  // $3.75: over the minimum size, under the $5 minimum notional
    s->evaluate(eth_view());
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "2499.75", "0.017")}));  // the buy keeps working
    gw->fill("0.001");  // $6.25 now: a valid sell
    s->evaluate(eth_view());
    auto last = actions(*gw).back();
    EXPECT_EQ(last.kind, "cancel");
    EXPECT_EQ(last.side, "BUY");
    EXPECT_TRUE(contains(gw->why, "switching to SELL")) << gw->why;
    gw->confirm_cancel();
    s->evaluate(eth_view());
    EXPECT_EQ(actions(*gw), (Actions {A("place", "SELL", "2500.27", "0.0025")}));  // (fair + tick) x (1 + markup), up
}

TEST(Strategy, ASellLeftUnderTheMinimumNotionalIsNotPlacedAgain) {
    auto [gw, s] = eth();
    s->evaluate(eth_view());
    gw->ack();
    gw->fill("0.017");
    s->evaluate(eth_view());
    auto sent = actions(*gw);
    EXPECT_EQ(sent.back().kind, "place");
    EXPECT_EQ(sent.back().side, "SELL");
    gw->ack();
    gw->fill("0.0155");  // 0.0015 ETH left, $3.75
    gw->confirm_cancel();  // pulled, e.g. re-priced: the rest alone would be rejected
    s->evaluate(eth_view());
    EXPECT_EQ(actions(*gw).back(), A("place", "BUY", "2499.75", "0.017"));  // buys more instead of a doomed sell
}

TEST(Strategy, AnOrderSizeUnderTheMinimumNotionalIsNeverPlaced) {
    LogCapture log("naive_mm.strategy");
    auto [gw, s] = eth(0.001);  // $2.50 at $2500: every placement would be rejected
    s->evaluate(eth_view());
    s->evaluate(eth_view());
    EXPECT_EQ(actions(*gw), Actions {});
    std::vector<std::string> found;
    for (const auto& m : log.messages())
        if (contains(m, "minimum notional")) found.push_back(m);
    EXPECT_EQ(found, (std::vector<std::string> {"order_size 0.001 at 2499.75 is under the minimum notional 5: not quoting, raise order_size"}));
}

TEST(Strategy, TheClosingSellTakesLessMarkup) {
    auto [gw, s] = make(0.001);
    StrategyOptions o = options(0.001, 0.0002, 0.001);
    o.close_markup_cut = 0.3;
    QuoteStrategy cut(gw.get(), btc_spec(), o);
    auto v = view(100'000.0, 99'990.0, 99'990.1);
    EXPECT_EQ(cut.price("BUY", v), D("99900.0"));  // the way in: as before
    EXPECT_EQ(s->price("BUY", v), D("99900.0"));
    EXPECT_EQ(s->price("SELL", v), D("100100.2"));  // (fair + tick) x 1.001, up
    EXPECT_EQ(cut.price("SELL", v), D("100070.2"));  // (fair + tick) x 1.0007, up: 30% less markup
}

TEST(Strategy, AnOrderTheOrdersChannelShowsCountsAsOursOnlyOnceTheBookCanShowIt) {
    // Run 14: OPEN came 1-5 ms before the book showed the order.
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    auto joined = book(100'000.0, {{99'990.0, 0.0005}, {99'989.9, 2.0}}, {{99'990.1, 1.0}});
    s->evaluate(joined);
    auto order = gw->order();
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));
    EXPECT_EQ(order->book_base, 0.0005);
    order->seen_open = true;  // OPEN, a moment before the book shows it
    order->open_at = monotonic();
    gw->set_state(*order, OrderState::OPEN);
    s->check_cancel(joined);
    EXPECT_EQ(actions(*gw), Actions {});  // the 0.0005 is someone else's: not alone, nothing to pull
    auto shown = book(100'000.0, {{99'990.0, 0.0015}, {99'989.9, 2.0}}, {{99'990.1, 1.0}});
    EXPECT_EQ(s->own_in_book("BUY", &shown.venue->bids), (std::optional<std::pair<double, double>>(std::pair {99'990.0, 0.001})));
    EXPECT_TRUE(order->book_seen);  // grew by ours
    order->book_seen = false;  // or once OPEN is old enough
    order->open_at = monotonic() - OPEN_IN_BOOK_S;
    EXPECT_EQ(s->own_in_book("BUY", &joined.venue->bids), (std::optional<std::pair<double, double>>(std::pair {99'990.0, 0.001})));
}

TEST(Strategy, NoNewOrderOnASideForAMomentAfterTheBookCameToOurs) {
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    FakeClock clock;
    s->clock = clock.fn();
    s->evaluate(book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    gw->ack();
    auto came = book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}});
    s->check_cancel(came);
    clock.advance(0.0335);  // the cancel's round trip (run 14: CANCELED 33.5 ms later, a reject 1 ms after it)
    gw->confirm_cancel();
    actions(*gw);
    auto after = book(100'000.0, {{99'989.9, 2.0}}, {{99'990.1, 1.0}});
    s->evaluate(after);
    EXPECT_EQ(actions(*gw), Actions {});  // the market is coming down: let it pass
    clock.advance(REPLACE_AFTER_CAME_S + 0.001);
    s->evaluate(after);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99989.9", "0.001")}));
}

TEST(Strategy, AfterTheBookCameToOursNoOrderGoesBackToThatPriceForCameHold) {
    // 18 Oct SOL/ETH: a best that flickers had the bot cancel and re-place at the same price.
    LogCapture log("naive_mm.orders");
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    s->came_hold_s = 0.3;
    FakeClock clock;
    s->clock = clock.fn();
    auto flicker = book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 2.0}}, {{99'990.1, 1.0}});
    s->evaluate(flicker);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));
    gw->ack();
    s->check_cancel(book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    clock.advance(0.03);
    gw->confirm_cancel();
    actions(*gw);
    s->evaluate(flicker);  // the level ahead shows again at once: within REPLACE_AFTER_CAME_S, nothing
    clock.advance(REPLACE_AFTER_CAME_S + 0.001);
    s->evaluate(flicker);  // then one tick behind the price the book came to, not back at it
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99989.9", "0.001")}));
    bool said = false;
    for (const auto& m : log.messages())
        said = said || contains(m, "the book came to our order at 99990 31 ms ago: one tick behind it for 300 ms");
    EXPECT_TRUE(said);
    EXPECT_EQ(s->came_hold("BUY"), D("99990.0"));
    EXPECT_FALSE(s->came_hold("SELL"));
    clock.advance(0.3);
    EXPECT_FALSE(s->came_hold("BUY"));
    EXPECT_EQ(s->price("BUY", flicker), D("99990.0"));  // over: back to the target
}

TEST(Strategy, WithoutCameHoldTheOrderGoesBackToThePriceTheBookCameTo) {
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    FakeClock clock;
    s->clock = clock.fn();
    auto flicker = book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 2.0}}, {{99'990.1, 1.0}});
    s->evaluate(flicker);
    gw->ack();
    s->check_cancel(book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    gw->confirm_cancel();
    actions(*gw);
    s->evaluate(flicker);
    clock.advance(REPLACE_AFTER_CAME_S + 0.001);
    s->evaluate(flicker);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99990.0", "0.001")}));
}

TEST(Strategy, AnOrderPlacedUnderTheThinSideMarkupIsBroughtBackByTolWhenThatEnds) {
    // ETH run 12: placed ~1 bp deeper under x5, then left there by tol_away.
    auto [gw, s] = make(0.0001, 0.00002);
    s->tol_away = D("0.001");  // 10 bp: the order would never come back on its own
    s->imbalance = Imbalance {5, 5.0, 5.0, 0.3, "open"};
    FakeClock clock;
    s->clock = clock.fn();
    auto thin_bids = book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 6.0}});
    s->evaluate(thin_bids);
    auto placed = actions(*gw);
    ASSERT_EQ(placed.size(), 1u);
    EXPECT_EQ(placed[0].price, D("99950.0"));  // x5 markup
    gw->ack();
    auto even = book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 2.0}});
    s->check_cancel(even);
    EXPECT_EQ(actions(*gw), Actions {});  // still held
    clock.advance(0.31);
    s->check_cancel(even);
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99950.0", "0.001")}));
    EXPECT_EQ(gw->why, "target 99990 is 4.00 bp away, the thin-side markup is over");
}

TEST(Strategy, TheFuturesSpotGapIsToldFromTheOrderSide) {
    EXPECT_EQ(gap_note(-0.5, -0.3, "BUY"), "futures -0.50 bp vs spot for it (-0.30 in 2 s)");  // futures below: bad for a buy
    EXPECT_EQ(gap_note(-0.5, std::nullopt, "SELL"), "futures +0.50 bp vs spot for it");
    EXPECT_FALSE(gap_note(std::nullopt, std::nullopt, "BUY"));
}

TEST(Strategy, ABookCameOrderThatFilledInsteadPausesNothingLater) {
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    FakeClock clock;
    s->clock = clock.fn();
    s->evaluate(book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    gw->ack();
    s->check_cancel(book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    auto sent = actions(*gw);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].kind, "place");
    EXPECT_EQ(sent[1].kind, "cancel");
    gw->fill("0.001");  // the taker got there first: filled, not cancelled
    s->evaluate(book(100'000.0, {{99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    sent = actions(*gw);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].kind + " " + sent[0].side, "place SELL");  // the sell goes out at once
    gw->ack();
    gw->fill("0.001");  // sold: flat again
    s->evaluate(book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 1.0}}));
    sent = actions(*gw);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].kind + " " + sent[0].side, "place BUY");  // no pause left over from the filled buy
}

TEST(Strategy, ThePauseAfterTheBookCameRunsInsideAGuardPause) {
    FakeClock clock;
    PriceGuard guard(0.01, 0.0001, 0.2, clock.fn());
    auto [gw, unused] = make(0.000005, 0.01);
    StrategyOptions o = options(0.000005, 0.01, 0.001);
    o.guards = {&guard};
    o.never_lead = true;
    QuoteStrategy s(gw.get(), btc_spec(), o);
    s.clock = clock.fn();
    s.evaluate(book(100'000.0, {{99'990.0, 1.0}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    gw->ack();
    s.check_cancel(book(100'000.0, {{99'990.0, 0.001}, {99'989.9, 2.0}}, {{99'990.1, 1.0}}));
    guard.until["BUY"] = clock() + 0.2;  // a drop pauses buying for 200 ms meanwhile
    clock.advance(0.0335);
    gw->confirm_cancel();
    actions(*gw);
    auto after = book(100'000.0, {{99'989.9, 2.0}}, {{99'990.1, 1.0}});
    s.evaluate(after);  // paused: nothing, but the 30 ms run from here
    clock.advance(0.2);  // the guard pause is over, long after the 30 ms
    s.evaluate(after);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99989.9", "0.001")}));
}

TEST(Strategy, AClosingOrderBiggerThanThePositionIsPulled) {
    // Review: the position can shrink under a working sell; filled whole, the sell would leave the bot short.
    auto [gw, s] = make();
    s->evaluate(view(100'000.0));
    gw->ack();
    gw->fill("0.001");
    s->evaluate(view(100'000.0));
    gw->ack();
    auto sent = actions(*gw);
    EXPECT_EQ(sent.back().kind + " " + sent.back().side, "place SELL");
    EXPECT_EQ(gw->order()->size, D("0.001"));
    gw->add_position(D("-0.0006"));  // 0.0006 sold elsewhere: 0.0004 long is left
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {Action {"cancel", "SELL", gw->order()->price, D("0.001")}}));
    EXPECT_EQ(gw->why, "0.001 left of it is more than the position 0.0004");
}

TEST(Strategy, AnOrderIsStillPulledWhileTheGatewayIsNotReady) {
    // Review: every cancel path waited for `ready`, which a reconnect takes away.
    NotReadyGateway gw;
    QuoteStrategy s(&gw, btc_spec(), options(0.001, 0.0002, 0.001));
    s.evaluate(view(100'000.0));
    gw.ack();
    actions(gw);
    gw.ready = false;
    s.evaluate(view(99'000.0));  // the fair fell 1%: the buy must go
    EXPECT_EQ(actions(gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));
    gw.confirm_cancel();
    s.evaluate(view(99'000.0));
    EXPECT_EQ(actions(gw), Actions {});  // but nothing new is placed until ready
}

TEST(Strategy, TheCameHoldOnASellKeepsItOneTickOver) {
    auto [gw, s] = make(0.000005, 0.01);
    s->never_lead = true;
    s->came_hold_s = 0.3;
    FakeClock clock;
    s->clock = clock.fn();
    QuoteStrategyTestAccess::came_price(*s)["SELL"] = D("99990.2");
    QuoteStrategyTestAccess::came_at(*s)["SELL"] = clock();
    auto v = book(99'980.0, {{99'990.0, 1.0}}, {{99'990.2, 1.0}, {99'990.3, 1.0}});
    EXPECT_EQ(s->price("SELL", v), D("99990.3"));  // the target 99990.2 is the held price: one tick over it
    QuoteStrategyTestAccess::came_price(*s)["SELL"] = D("99990.1");
    EXPECT_EQ(s->price("SELL", v), D("99990.2"));  // a target over the held price is left alone
}

// ------------------------------------------------------------- from test_guard.py

TEST(Strategy, PlacesNothingOnAPausedSide) {
    auto [gw, s] = make(0.001, 0.0002);
    auto rise = tripped("SELL");
    s->guards = {rise.get()};
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.001")}));  // a rise does not stop buying
    gw->confirm_cancel();
    auto drop = tripped("BUY");
    s->guards = {drop.get()};
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});  // a drop does
    drop->until["BUY"] = 0.0;  // the pause is over
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.001")}));
}

TEST(Strategy, PullCancelsTheOrderOnTheThreatenedSideWhateverItsPrice) {
    auto [gw, s] = make(0.001, 0.0002);
    s->pull("BUY", "drop");
    EXPECT_EQ(actions(*gw), Actions {});  // nothing to pull
    s->evaluate(view(100'000.0));
    actions(*gw);
    s->pull("SELL", "rise");
    EXPECT_EQ(actions(*gw), Actions {});  // a rise leaves a buy alone
    s->pull("BUY", "drop");  // before the ack, too
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.001")}));
    s->pull("BUY", "drop");
    EXPECT_EQ(actions(*gw), Actions {});  // already being cancelled
}
