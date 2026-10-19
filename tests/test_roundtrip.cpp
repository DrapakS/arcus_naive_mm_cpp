// Round trips, one log line each (the RoundTrips-level tests of tests/test_roundtrip.py; the ones that
// go through the Arcus gateway are with the gateway).
#include <gtest/gtest.h>

#include "test_util.hpp"
#include "trading/roundtrip.hpp"
#include "util/log.hpp"

using nmm::Decimal;
using nmm::RoundTrips;
using nmm::test::FakeClock;

namespace {

Decimal D(const char* s) { return Decimal(s); }

struct Capture {
    std::shared_ptr<nmm::logging::CaptureHandler> handler = std::make_shared<nmm::logging::CaptureHandler>();
    nmm::Logger& logger = nmm::get_logger(nmm::ORDERS_LOGGER);
    int level = logger.level();
    Capture() {
        logger.set_level(nmm::logging::INFO);
        logger.add_handler(handler);
    }
    ~Capture() {
        logger.remove_handler(handler);
        logger.set_level(level);
    }
    std::vector<std::string> starting(const std::string& prefix) {
        std::vector<std::string> out;
        for (auto& m : handler->messages())
            if (m.starts_with(prefix)) out.push_back(m);
        return out;
    }
    std::vector<std::string> lines() { return starting("=== ROUND TRIP"); }
};

struct Trips {
    FakeClock clock;
    RoundTrips rt;
    explicit Trips(Decimal dust = 0) : rt(nmm::get_logger(nmm::ORDERS_LOGGER), clock.fn(), dust) {}
};

}  // namespace

TEST(RoundTrips, ALongBoughtInPartsAndSoldIsOneTrip) {
    Capture capture;
    Trips t;
    auto& [clock, rt] = t;
    rt.fill("BUY", D("0.0002"), Decimal(100000));
    clock.advance(1.0);
    rt.fill("BUY", D("0.0002"), Decimal(100010));  // average 100005
    rt.mark(99'995.0, 99'995.1);  // worth 99995: -1 bp of what was bought
    clock.advance(1.5);
    rt.mark(100'020.0, 100'020.1);
    rt.fill("SELL", D("0.0001"), Decimal(100020));
    EXPECT_TRUE(rt.open());
    EXPECT_EQ(rt.position(), D("0.0003"));
    clock.advance(0.5);
    rt.fill("SELL", D("0.0003"), Decimal(100030));
    EXPECT_FALSE(rt.open());
    EXPECT_EQ(rt.position(), Decimal(0));
    // bought 0.0004 for $40.002, sold for $40.011: +$0.009, 2.25 bp
    EXPECT_EQ(capture.lines(), (std::vector<std::string> {
                                   "=== ROUND TRIP #1 long 0.0004: bought @ 100005.00, sold @ 100027.50, held 3.0 s, "
                                   "+2.25 bp ($+0.0090); worst -1.00 bp at 1.0 s; 4 fills; all 1 trips $+0.0090 (+2.25 bp)"}));
    EXPECT_EQ(rt.status(), "round trips 1, $+0.0090 (+2.25 bp)");
}

TEST(RoundTrips, AShortIsMarkedAtTheAsk) {
    Trips t;
    auto& rt = t.rt;
    rt.fill("SELL", D("0.001"), Decimal(100000));
    rt.mark(100'009.9, 100'010.0);  // buying it back costs the ask: -1 bp
    EXPECT_NEAR(rt.trip->worst_bps, -1.0, 1e-6);
    rt.fill("BUY", D("0.001"), Decimal(99990));
    EXPECT_EQ(rt.count, 1);
    EXPECT_EQ(rt.pnl, D("0.01"));
}

TEST(RoundTrips, AFillThroughZeroClosesTheTripAndOpensTheNext) {
    Capture capture;
    Trips t;
    auto& rt = t.rt;
    rt.fill("BUY", D("0.001"), Decimal(100000));
    rt.fill("SELL", D("0.0015"), Decimal(100010));
    EXPECT_EQ(capture.lines().size(), 1u);
    EXPECT_EQ(rt.count, 1);
    ASSERT_TRUE(rt.open());
    EXPECT_EQ(rt.trip->side, "SELL");
    EXPECT_EQ(rt.position(), D("-0.0005"));
}

TEST(RoundTrips, APositionWeDidNotBuildIsNoTrip) {
    Capture capture;
    Trips t;
    auto& rt = t.rt;
    rt.sync(D("0.0003"));  // found at start
    rt.fill("SELL", D("0.0003"), Decimal(100000));  // sold off: no trip, back to tracking
    EXPECT_TRUE(capture.lines().empty());
    EXPECT_FALSE(rt.open());
    EXPECT_EQ(rt.position(), Decimal(0));
    rt.fill("BUY", D("0.0001"), Decimal(100000));
    rt.sync(D("0.0002"));  // a leftover order filled: the trip cannot be told any more
    EXPECT_FALSE(rt.open());
    EXPECT_EQ(rt.untracked, D("0.0002"));
    EXPECT_EQ(capture.lines(), (std::vector<std::string> {"=== ROUND TRIP dropped: position 0.0002, our fills make 0.0001"}));
    rt.sync(D("0.0002"));  // in agreement: nothing to do
    EXPECT_EQ(rt.untracked, D("0.0002"));
}

TEST(RoundTrips, ALongFoundAtStartTooSmallToSellDoesNotSwallowTheNextTrip) {
    Capture capture;
    Trips t;
    auto& rt = t.rt;
    rt.sync(D("0.0015"));  // ETH worth ~$3.75 left by the last run: the bot buys on top of it
    rt.fill("BUY", D("0.017"), Decimal(2500));
    EXPECT_TRUE(rt.open());
    EXPECT_EQ(rt.untracked, D("0.0015"));
    EXPECT_EQ(rt.position(), D("0.0185"));
    rt.fill("SELL", D("0.0185"), D("2500.25"));  // the whole position goes in one sell
    EXPECT_FALSE(rt.open());
    EXPECT_EQ(rt.untracked, Decimal(0));
    EXPECT_TRUE(capture.lines()[0].starts_with("=== ROUND TRIP #1 long 0.017: bought @ 2500.00, sold @ 2500.25"));
}

TEST(RoundTrips, TheTripLineTellsHowTheExitWent) {
    Capture capture;
    Trips t;
    auto& [clock, rt] = t;
    rt.placed("BUY", Decimal(100000), 100'000.05);  // the entry: not counted as an exit
    rt.fill("BUY", D("0.001"), Decimal(100000));
    rt.placed("SELL", Decimal(100010), 100'000.0);  // first sell 1 bp over the mid
    rt.mark(100'000.0, 100'000.1, "SELL", 100'010.0);  // behind the best ask
    clock.advance(3.0);
    rt.mark(100'000.0, 100'000.1, "SELL", 100'010.0);
    rt.placed("SELL", Decimal(100005), 100'004.95);  // re-placed lower
    rt.mark(100'004.9, 100'005.0, "SELL", 100'005.0);  // at the best ask
    clock.advance(1.0);
    rt.mark(100'004.9, 100'005.0, "SELL", 100'005.0);
    rt.fill("SELL", D("0.001"), Decimal(100005));
    auto line = capture.lines()[0];
    EXPECT_NE(line.find("; 2 fills; exit: 2 sells, the first +1.00 bp from the mid, at the best ask 25% of 4.0 s; all 1 trips"),
              std::string::npos)
        << line;
}

TEST(RoundTrips, ACrumbUnderTheMinimumSizeClosesTheTrip) {
    Capture capture;
    Trips t(D("0.0001"));
    auto& rt = t.rt;
    rt.fill("BUY", D("0.001"), Decimal(100000));
    rt.fill("SELL", D("0.00099967"), Decimal(100010));  // 3.3e-7 left: it cannot be sold on its own
    EXPECT_FALSE(rt.open());
    EXPECT_EQ(rt.untracked, D("0.00000033"));
    EXPECT_EQ(rt.position(), D("0.00000033"));
    auto line = capture.lines()[0];
    EXPECT_TRUE(line.starts_with(
        "=== ROUND TRIP #1 long 0.00099967: bought @ 100000.00, sold @ 100010.00, held 0.0 s, +1.00 bp"))
        << line;
    EXPECT_NE(line.find("; 2 fills; 0.00000033 left over, under the minimum size; all 1 trips"), std::string::npos) << line;
}

TEST(RoundTrips, APositionFoundAtStartIsValuedAtTheExchangeEntryWhenItCloses) {
    Capture capture;
    Trips t;
    auto& rt = t.rt;
    rt.sync(D("0.0005"), Decimal(100000));
    rt.fill("SELL", D("0.0005"), Decimal(99990));
    EXPECT_EQ(capture.starting("==="),
              (std::vector<std::string> {
                  "=== position 0.0005 set aside: not a round trip of this run (the exchange's average entry 100000)",
                  "=== SET-ASIDE position closed: sold 0.0005 @ 99990.00, entry 100000: -1.00 bp ($-0.0050)"}));
}

TEST(RoundTrips, WhatIsOpenAtShutdownIsMarked) {
    Capture capture;
    Trips t;
    auto& [clock, rt] = t;
    rt.report_final(100'000.0, 100'000.1);
    rt.fill("BUY", D("0.001"), Decimal(100000));
    clock.advance(12.0);
    rt.report_final(99'990.0, 99'990.1);
    EXPECT_EQ(capture.starting("=== AT EXIT"),
              (std::vector<std::string> {"=== AT EXIT: flat",
                                         "=== AT EXIT: long 0.001 of a round trip open 12.0 s, bought @ 100000.00, worth "
                                         "99990.00 at the best bid: -1.00 bp ($-0.0100)"}));
}

TEST(RoundTrips, AtExitWithoutPricesTheLastTopSeenCountsAndASetAsideIsNamed) {
    Capture capture;
    Trips t;
    auto& [clock, rt] = t;
    rt.sync(D("-0.0008"));  // a short found at start
    EXPECT_FALSE(rt.open());
    rt.fill("SELL", D("0.001"), Decimal(100000));  // adds to it: a trip of our own
    rt.mark(99'990.0, 100'010.0);
    clock.advance(2.0);
    rt.report_final();
    EXPECT_EQ(capture.starting("=== AT EXIT").back(),
              "=== AT EXIT: short 0.001 of a round trip open 2.0 s, sold @ 100000.00, worth 100010.00 at the best ask: "
              "-1.00 bp ($-0.0100); -0.0008 set aside");
}

TEST(RoundTrips, ACrumbStillWorkedByTheExitOrderEndsTheTripOnlyWithThatOrder) {
    Capture capture;
    Trips t(D("0.001"));
    auto& rt = t.rt;
    rt.fill("BUY", D("0.017"), Decimal(2500));
    rt.fill("SELL", D("0.0165"), D("2500.5"), true);  // 0.0005 left on the same sell order
    EXPECT_TRUE(rt.open());
    EXPECT_TRUE(capture.lines().empty());
    rt.fill("SELL", D("0.0005"), D("2500.5"));  // the same order sells the crumb too
    EXPECT_FALSE(rt.open());
    EXPECT_EQ(rt.untracked, Decimal(0));
    ASSERT_EQ(capture.lines().size(), 1u);
    EXPECT_NE(capture.lines()[0].find("long 0.017: bought @ 2500.00, sold @ 2500.50"), std::string::npos);
    rt.fill("BUY", D("0.017"), Decimal(2500));
    rt.fill("SELL", D("0.0165"), D("2500.5"), true);
    rt.settle("SELL");  // that sell is cancelled with the crumb unsold: the trip ends, the crumb set aside
    EXPECT_FALSE(rt.open());
    EXPECT_EQ(rt.untracked, D("0.0005"));
    EXPECT_EQ(rt.aside_entry, Decimal(2500));
    EXPECT_NE(capture.lines()[1].find("0.0005 left over, under the minimum size"), std::string::npos);
}

TEST(RoundTrips, StatusShowsTheOpenTrip) {
    Trips t;
    auto& [clock, rt] = t;
    rt.fill("BUY", D("0.001"), Decimal(100000));
    rt.mark(99'990.0, 99'990.1);
    clock.advance(3.25);
    EXPECT_EQ(rt.status(), "round trips 0, $+0.0000 (+0.00 bp), one open since 3.2 s (worst -1.00 bp)");
}
