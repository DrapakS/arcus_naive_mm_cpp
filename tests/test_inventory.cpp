// Ported from tests/test_inventory.py: both sides' inventory skew, passive exit urgency, the add
// guard, and where a position's entry is kept.
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

std::vector<std::pair<std::string, Decimal>> side_sizes(const Actions& actions) {
    std::vector<std::pair<std::string, Decimal>> out;
    for (const auto& a : actions) out.emplace_back(a.side, a.size);
    return out;
}

}  // namespace

TEST(Inventory, TheGatewayKeepsTheEntryAndSinceWhenThePositionIsHeld) {
    FakeGateway gw;
    FakeClock clock(100.0);
    gw.clock = clock.fn();
    auto buy = gw.place("BUY", Decimal(100), Decimal(2));
    gw.fill(buy, Decimal(1), std::string("100"));
    EXPECT_EQ(gw.entry_price, Decimal(100));
    EXPECT_EQ(gw.position_since, 100.0);
    clock.advance(5);
    gw.fill(buy, Decimal(2), std::string("103"));  // added to: the mean of what is held
    EXPECT_EQ(gw.entry_price, D("101.5"));
    EXPECT_EQ(gw.position_since, 100.0);
    auto sell = gw.place("SELL", Decimal(110), Decimal(3));
    gw.fill(sell, Decimal(1), std::string("110"));  // reduced: the entry stands
    EXPECT_EQ(gw.entry_price, D("101.5"));
    clock.advance(5);
    gw.fill(sell, Decimal(3), std::string("111"));  // through zero to a short of 1: a new position from now
    EXPECT_EQ(gw.position(), Decimal(-1));
    EXPECT_EQ(gw.entry_price, Decimal(111));
    EXPECT_EQ(gw.position_since, 110.0);
    gw.found_position(Decimal(90));  // a position the exchange reports keeps what our fills told
    EXPECT_EQ(gw.entry_price, Decimal(111));
    gw.add_position(Decimal(1));
    gw.found_position(std::nullopt);  // flat
    EXPECT_FALSE(gw.entry_price);
    EXPECT_FALSE(gw.position_since);
}

TEST(Inventory, ThePositionSkewsBothQuotes) {
    auto plain_rig = both(0.001, 0.0002, SIZE, 3000.0, [](StrategyOptions& o) { o.close_markup_cut = 0.3; });
    auto plain = plain_rig.s->targets(view(100'000.0), D("0.015"));
    auto [gw, s] = both(0.001, 0.0002, SIZE, 3000.0, [](StrategyOptions& o) {
        o.close_markup_cut = 0.3;
        o.skew_open = 1.0;
        o.skew_close = 1.0;
    });
    gw->add_position(D("0.015"));  // $1500 of $3000 long: u = 0.5 at the fair
    auto skewed = s->targets(view(100'000.0), D("0.015"));
    // the buy adds to the long: deeper; the sell closes it: nearer the market
    EXPECT_LT(skewed["BUY"]->price, plain["BUY"]->price);
    EXPECT_LT(skewed["SELL"]->price, plain["SELL"]->price);
    EXPECT_EQ(skewed["BUY"]->price, D("99850.0"));  // 100000 * (1 - 0.001 * (1 + 0.5))
    EXPECT_EQ(skewed["SELL"]->price, D("100035.2"));  // 100000.1 * (1 + 0.0007 * (1 - 0.5)) = 100035.10..., up to the tick
}

TEST(Inventory, APositionHeldTooLongOrUnderWaterClosesAtTheBestPrice) {
    LogCapture log("naive_mm.strategy");
    auto [gw, s] = both(0.001, 0.0002, SIZE, 3000.0, [](StrategyOptions& o) {
        o.close_markup_cut = 0.3;
        o.exit_after_s = 600;
        o.exit_loss = 0.0005;
    });
    FakeClock clock(0.0);
    gw->clock = clock.fn();
    s->evaluate(view(100'000.0));
    actions(*gw);
    gw->ack("BUY");
    gw->ack("SELL");
    gw->fill("BUY", "0.0099");  // long at 99900
    EXPECT_EQ(s->price("SELL", view(100'000.0)), D("100070.2"));  // the closing markup as usual
    clock.advance(600);
    EXPECT_EQ(s->price("SELL", view(100'000.0)), D("99990.1"));  // held 600 s: joins the best ask
    EXPECT_TRUE(contains(log.messages().back(), "held 600 s: closing at the best price, no markup")) << log.messages().back();
    EXPECT_EQ(s->price("SELL", view(99'000.0)), D("99990.1"));  // a fair under the bid: still never crosses
    auto [gw2, s2] = both(0.001, 0.0002, SIZE, 3000.0, [](StrategyOptions& o) { o.exit_loss = 0.0005; });
    s2->evaluate(view(100'000.0));
    actions(*gw2);
    gw2->ack("BUY");
    gw2->fill("BUY", "0.0099");  // long at 99900
    EXPECT_EQ(s2->price("SELL", view(100'000.0)), D("100100.2"));
    auto under = view(100'000.0, 99'850.0, 99'850.1);  // the mid 5 bp under the entry
    EXPECT_EQ(s2->price("SELL", under), D("99850.1"));
    s2->never_lead = true;  // with never_lead it joins the others' best ask
    EXPECT_EQ(s2->price("SELL", book(100'000.0, {{99'850.0, 1.0}}, {{99'850.1, 1.0}, {99'850.5, 1.0}})), D("99850.1"));
}

TEST(Inventory, AfterAMoveAgainstItASideAddsUpToAShareOfTheLimitOnly) {
    LogCapture log("naive_mm.strategy");
    FakeClock clock;
    TrendGuard guard(300.0, 0.0005, 60.0, "both", clock.fn());
    auto [gw, s] = both(0.001, 0.0002, SIZE, 3000.0, [&](StrategyOptions& o) {
        o.add_guard = &guard;
        o.add_limit_share = 0.5;
    });
    s->evaluate(view(100'000.0));  // flat: both sides, whole orders
    EXPECT_EQ(side_sizes(actions(*gw)), (std::vector<std::pair<std::string, Decimal>> {{"BUY", D("0.0099")}, {"SELL", D("0.0099")}}));
    gw->ack("BUY");
    gw->ack("SELL");
    gw->fill("BUY", "0.0099");  // a $990 long, then a buy of $990 more is working again
    s->evaluate(view(100'000.0));
    actions(*gw);
    gw->ack("BUY");
    guard.observe(100'000.0);
    clock.advance(10);
    EXPECT_TRUE(guard.observe(99'940.0));  // -6 bp: buys are held to 50% of the limit
    s->check_cancel(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.0099")}));
    EXPECT_TRUE(contains(log.messages().back(), "buying at most to 50% of the limit")) << log.messages().back();
    EXPECT_TRUE(contains(gw->why, "more than the 0.00511501 wanted now")) << gw->why;  // 1500 / 99900 - 0.0099
    gw->finish(gw->orders.at("BUY"), OrderState::CANCELED);
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.00511501")}));
    clock.advance(61);  // the pause is over: whole orders again
    s->check_cancel(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});  // (a smaller order is left alone)
    EXPECT_EQ(s->targets(view(100'000.0), gw->position())["BUY"]->size, D("0.0099"));
}

TEST(Inventory, TheClosingOrderIsTheBiggerOfThePositionAndOrderSize) {
    auto [gw, s] = both(0.001, 0.0002, SIZE, 3000.0);
    gw->add_position(D("0.004"));  // under one order: a whole order closes it, through zero
    EXPECT_EQ(s->targets(view(100'000.0), gw->position())["SELL"]->size, D("0.0099"));
    gw->add_position(D("0.012"));  // over one order: the whole position in one order
    EXPECT_EQ(s->targets(view(100'000.0), gw->position())["SELL"]->size, D("0.016"));
    s->evaluate(view(100'000.0));
    actions(*gw);
    gw->ack("BUY");
    gw->ack("SELL");
    gw->fill("BUY", "0.0099");  // the long grew past the working sell: it goes, for one of the whole position
    s->check_cancel(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "SELL", "100100.2", "0.016")}));
    EXPECT_EQ(gw->why, "0.016 left of it is less than the position 0.0259");
    gw->finish(gw->orders.at("SELL"), OrderState::CANCELED);
    s->evaluate(view(100'000.0));
    std::vector<Decimal> sells;
    for (const auto& a : actions(*gw))
        if (a.kind == "place" && a.side == "SELL") sells.push_back(a.size);
    EXPECT_EQ(sells, (std::vector<Decimal> {D("0.0259")}));
}

TEST(Inventory, AnOrderAFewSatoshiOverASizeThatFollowsThePriceIsLeftAlone) {
    FakeClock clock;
    TrendGuard guard(300.0, 0.0005, 60.0, "both", clock.fn());
    auto [gw, s] = both(0.001, 0.0002, SIZE, 3000.0, [&](StrategyOptions& o) {
        o.add_guard = &guard;
        o.add_limit_share = 0.5;
    });
    gw->add_position(D("0.0099"));
    guard.observe(100'000.0);
    clock.advance(10);
    guard.observe(99'940.0);  // buys held to 50% of the limit: $1500 at the buy's price
    s->evaluate(view(100'000.0));
    EXPECT_EQ(side_sizes(actions(*gw)), (std::vector<std::pair<std::string, Decimal>> {{"BUY", D("0.00511501")}, {"SELL", D("0.0099")}}));
    gw->ack("BUY");
    gw->ack("SELL");
    s->check_cancel(view(100'001.0));  // a tick up: the share is a few satoshi smaller, the order stays
    EXPECT_EQ(actions(*gw), Actions {});
}
