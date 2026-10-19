// The guards themselves (tests/test_guard.py, and the guard-level parts of test_drift.py,
// test_lag.py and test_take_guard.py); the strategy and agent sides of them are elsewhere.
#include <gtest/gtest.h>

#include <cmath>

#include "test_util.hpp"
#include "trading/guard.hpp"

using nmm::DriftWatch;
using nmm::LagGuard;
using nmm::PriceGuard;
using nmm::SweepGuard;
using nmm::TakeGuard;
using nmm::TrendGuard;
using nmm::test::FakeClock;

namespace {

::testing::AssertionResult approx(double actual, double expected, double rel = 1e-6, double abs_ = 1e-12) {
    if (std::fabs(actual - expected) <= std::max(rel * std::fabs(expected), abs_)) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << actual << " is not approximately " << expected;
}

std::map<std::string, int64_t> counts(int64_t buy, int64_t sell) { return {{"BUY", buy}, {"SELL", sell}}; }

struct Guard {
    FakeClock clock;
    PriceGuard g;
    explicit Guard(double window_s = 1.0, double pause_s = 0.1) : g(window_s, 0.0001, pause_s, clock.fn()) {}
};

}  // namespace

TEST(PriceGuard, DropUnderTheTimeWeightedMeanThreatensBuys) {
    Guard t;
    auto& [clock, g] = t;
    EXPECT_FALSE(g.observe(100'000.0));
    clock.advance(0.5);
    EXPECT_FALSE(g.observe(99'900.0));  // half a window seen so far: nothing to compare with
    clock.advance(0.5);
    // over the last second: 100000 for 0.5 s, 99900 for 0.5 s -> 99950
    EXPECT_TRUE(approx(*g.mean(), 99'950.0));
    EXPECT_FALSE(g.observe(99'941.0));  // 0.9 bp under
    EXPECT_FALSE(g.paused("BUY"));
    auto trip = g.observe(99'939.0);  // 1.1 bp under; the 99941 before it stood for no time
    ASSERT_TRUE(trip);
    EXPECT_TRUE(trip->side == "BUY" && trip->drop() && trip->fresh);
    EXPECT_TRUE(approx(trip->mean, 99'950.0));
    EXPECT_TRUE(g.paused("BUY") && !g.paused("SELL"));
    EXPECT_EQ(g.trips, counts(1, 0));
}

TEST(PriceGuard, EachPriceCountsForAsLongAsItStood) {
    Guard t;
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(0.9);
    g.observe(100'100.0);  // a burst of updates at a new price weighs by time, not by count
    for (int i = 0; i < 50; ++i) g.observe(100'100.0);
    clock.advance(0.1);
    EXPECT_TRUE(approx(*g.mean(), 100'010.0));
    clock.advance(0.95);  // 100000 is out of the window now
    EXPECT_TRUE(approx(*g.mean(), 100'100.0));
}

TEST(PriceGuard, RiseOverTheMeanThreatensSells) {
    Guard t;
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(1.0);
    EXPECT_FALSE(g.observe(100'009.0));  // 0.9 bp over: within drawdown
    auto trip = g.observe(100'011.0);
    ASSERT_TRUE(trip);
    EXPECT_TRUE(trip->side == "SELL" && !trip->drop());
    EXPECT_TRUE(approx(trip->mean, 100'000.0));
    EXPECT_TRUE(g.paused("SELL") && !g.paused("BUY"));
    EXPECT_EQ(g.trips, counts(0, 1));
}

TEST(PriceGuard, PauseRunsFromTheLastMoveAndCountsOneTrip) {
    Guard t(1.0, 0.05);
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(1.0);
    EXPECT_TRUE(g.observe(99'000.0)->fresh);
    double first = g.until["BUY"];
    clock.advance(0.01);
    auto trip = g.observe(98'000.0);  // still falling: the pause runs on
    ASSERT_TRUE(trip);
    EXPECT_FALSE(trip->fresh);
    EXPECT_GT(g.until["BUY"], first);
    EXPECT_EQ(g.trips["BUY"], 1);
    EXPECT_EQ(g.extended["BUY"], 1);
    EXPECT_TRUE(approx(g.until["BUY"] - g.since["BUY"], 0.06));  // for the logs
    clock.advance(0.06);
    EXPECT_FALSE(g.paused("BUY"));
}

TEST(PriceGuard, ResetNeedsAFullWindowAgain) {
    Guard t;
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(1.0);
    g.reset();
    EXPECT_FALSE(g.observe(90'000.0));
    EXPECT_FALSE(g.paused("BUY"));
    clock.advance(0.5);
    EXPECT_FALSE(g.observe(80'000.0));  // half a window since the reset
    clock.advance(0.5);
    EXPECT_TRUE(g.observe(80'000.0)->drop());  // mean 85000 over the last second
}

TEST(PriceGuard, LongRunKeepsTheMeanExact) {
    Guard t(0.5);
    auto& [clock, g] = t;
    for (int i = 0; i < 5'000; ++i) {
        clock.advance(0.01);
        g.observe(100'000.0 + (i % 7));
    }
    // the last half second holds prices 4949..4998; the newest has stood for no time yet
    double expected = 0.0;
    for (int i = 4'949; i < 4'999; ++i) expected += 100'000.0 + (i % 7);
    expected /= 50;
    EXPECT_TRUE(approx(*g.mean(), expected, 1e-6, 1e-6));
    EXPECT_LE(g.steps.size(), 52u);  // only the window is kept
}

TEST(PriceGuard, RejectsBadSettings) {
    EXPECT_THROW(PriceGuard(0.0, 0.0001, 0.1), std::invalid_argument);
    EXPECT_THROW(PriceGuard(1.0, 0.0, 0.1), std::invalid_argument);
}

namespace {
struct Trend {
    FakeClock clock;
    TrendGuard g;
    explicit Trend(std::string side = "BUY", double window_s = 120.0, double pause_s = 60.0)
        : g(window_s, 0.003, pause_s, std::move(side), clock.fn()) {}
};
}  // namespace

TEST(TrendGuard, HoldsBuysOffAfterAFallFromTheWindowHigh) {
    Trend t;
    auto& [clock, g] = t;
    EXPECT_FALSE(g.observe(100'000.0));
    clock.advance(10);
    EXPECT_FALSE(g.observe(100'100.0));  // the high so far
    clock.advance(10);
    EXPECT_FALSE(g.observe(99'900.0));  // 20 bp under it: not yet
    EXPECT_FALSE(g.paused("BUY"));
    clock.advance(10);
    auto trip = g.observe(99'780.0);  // 32 bp under 100100
    ASSERT_TRUE(trip);
    EXPECT_TRUE(trip->side == "BUY" && trip->drop() && trip->fresh && trip->mean == 100'100.0);
    EXPECT_TRUE(g.paused("BUY") && !g.paused("SELL"));
    EXPECT_EQ(g.trips, counts(1, 0));
    EXPECT_EQ(g.reference, "high");
    EXPECT_EQ(g.extreme(), 100'100.0);
}

TEST(TrendGuard, PauseRunsOnWhileThePriceStaysDownThenEnds) {
    Trend t("BUY", 120.0, 60.0);
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(30);
    EXPECT_TRUE(g.observe(99'600.0)->fresh);  // 40 bp down
    clock.advance(30);
    auto trip = g.observe(99'650.0);  // a little back up, still 35 bp under the high: the pause runs on
    ASSERT_TRUE(trip);
    EXPECT_FALSE(trip->fresh);
    EXPECT_EQ(g.extended["BUY"], 1);
    EXPECT_TRUE(approx(g.until["BUY"], clock() + 60));
    clock.advance(61);  // the high is 121 s old: out of the window, and no price that far down since
    EXPECT_FALSE(g.paused("BUY"));
    EXPECT_FALSE(g.observe(99'650.0));
    EXPECT_EQ(g.extreme(), 99'650.0);
}

TEST(TrendGuard, ForgetsHighsOlderThanItsWindow) {
    Trend t("BUY", 120.0);
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(60);
    g.observe(99'900.0);
    clock.advance(61);  // 100000 is 121 s old
    EXPECT_EQ(g.extreme(), 99'900.0);
    EXPECT_FALSE(g.observe(99'650.0));  // 25 bp under what is left of the window
    clock.advance(1);
    g.observe(99'800.0);  // 99650 can no longer be the high: only what still can is kept
    std::vector<double> prices;
    for (auto& [_, p] : g.windows["BUY"]) prices.push_back(p);
    EXPECT_EQ(prices, (std::vector<double> {99'900.0, 99'800.0}));
}

TEST(TrendGuard, OfAShortWaitsAfterARallyOverTheWindowLow) {
    Trend t("SELL");
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(20);
    EXPECT_FALSE(g.observe(99'900.0));  // the low so far
    clock.advance(20);
    auto trip = g.observe(100'210.0);  // 31 bp over 99900
    ASSERT_TRUE(trip);
    EXPECT_TRUE(trip->side == "SELL" && !trip->drop() && trip->mean == 99'900.0);
    EXPECT_TRUE(g.paused("SELL") && !g.paused("BUY"));
    EXPECT_EQ(g.reference, "low");
    clock.advance(61);
    EXPECT_FALSE(g.observe(99'000.0));  // a fall never holds a short's closing buy back
}

TEST(TrendGuard, ResetAndBadSettings) {
    Trend t;
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(1);
    g.reset();
    EXPECT_FALSE(g.extreme());
    EXPECT_FALSE(g.observe(99'000.0));
    EXPECT_THROW(TrendGuard(0.0, 0.003, 60.0, "BUY"), std::invalid_argument);
    EXPECT_THROW(TrendGuard(120.0, 0.0, 60.0, "BUY"), std::invalid_argument);
    EXPECT_THROW(TrendGuard(120.0, 0.003, -1.0, "BUY"), std::invalid_argument);
    EXPECT_THROW(TrendGuard(120.0, 0.003, 60.0, "HOLD"), std::invalid_argument);
}

TEST(TrendGuard, BothSidesHoldBuysOnAFallAndSellsOnARally) {
    FakeClock clock;
    TrendGuard g(120.0, 0.003, 60.0, "both", clock.fn());
    EXPECT_EQ(g.reference, "high or low");
    EXPECT_EQ(g.reference_of("BUY"), "high");
    EXPECT_EQ(g.reference_of("SELL"), "low");
    g.observe(100'000.0);
    clock.advance(10);
    auto trip = g.observe(99'600.0);
    ASSERT_TRUE(trip);
    EXPECT_EQ(trip->side, "BUY");
    EXPECT_TRUE(g.paused("BUY") && !g.paused("SELL"));
    clock.advance(10);
    trip = g.observe(100'000.0);  // 40 bp over the low 99600
    ASSERT_TRUE(trip);
    EXPECT_EQ(trip->side, "SELL");
    EXPECT_TRUE(g.paused("SELL"));
}

namespace {
struct Sweep {
    FakeClock clock;
    SweepGuard g;
    explicit Sweep(double window_s = 0.3, double pause_s = 0.2) : g(window_s, 0.00003, pause_s, clock.fn()) {}
};
}  // namespace

TEST(SweepGuard, TripsOnATradeThroughTheHighOrTheLowOfItsWindow) {
    Sweep t;
    auto& [clock, g] = t;
    EXPECT_FALSE(g.observe(100'000.0));  // nothing before it
    clock.advance(0.1);
    EXPECT_FALSE(g.observe(100'002.0));  // 0.2 bp over the low: not yet
    clock.advance(0.1);
    auto trip = g.observe(99'998.0);  // 0.4 bp under the high 100002
    ASSERT_TRUE(trip);
    EXPECT_TRUE(trip->side == "BUY" && trip->drop() && trip->fresh && trip->mean == 100'002.0);
    EXPECT_TRUE(g.paused("BUY") && !g.paused("SELL"));
    clock.advance(0.01);
    trip = g.observe(100'002.5);  // 0.45 bp over the low 99998: a rise, the other side
    ASSERT_TRUE(trip);
    EXPECT_TRUE(trip->side == "SELL" && trip->mean == 99'998.0 && g.paused("SELL"));
    EXPECT_EQ(g.trips, counts(1, 1));
    EXPECT_EQ(g.extremes(), std::make_pair(100'002.5, 99'998.0));
}

TEST(SweepGuard, KeepsOnlyWhatCanStillBeTheHighOrTheLow) {
    Sweep t;
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(0.1);
    g.observe(99'999.0);
    clock.advance(0.1);
    g.observe(99'999.5);
    std::vector<double> highs, lows;
    for (auto& [_, p] : g.highs) highs.push_back(p);
    for (auto& [_, p] : g.lows) lows.push_back(p);
    EXPECT_EQ(highs, (std::vector<double> {100'000.0, 99'999.5}));
    EXPECT_EQ(lows, (std::vector<double> {99'999.0, 99'999.5}));
    clock.advance(0.11);  // 100000 is 310 ms old: out of the window
    EXPECT_EQ(g.extremes(), std::make_pair(99'999.5, 99'999.0));
    EXPECT_FALSE(g.observe(99'997.0));  // 0.25 bp under what is left of the window
    g.reset();
    EXPECT_FALSE(g.extremes());
    EXPECT_FALSE(g.observe(90'000.0));
}

TEST(SweepGuard, PauseRunsOnWhileTradesPrintThatFarOut) {
    Sweep t(0.3, 0.2);
    auto& [clock, g] = t;
    g.observe(100'000.0);
    clock.advance(0.05);
    EXPECT_TRUE(g.observe(99'995.0)->fresh);
    clock.advance(0.1);
    auto trip = g.observe(99'994.0);  // still 0.6 bp under the high
    ASSERT_TRUE(trip);
    EXPECT_FALSE(trip->fresh);
    EXPECT_EQ(g.extended["BUY"], 1);
    EXPECT_TRUE(approx(g.until["BUY"], clock() + 0.2));
    clock.advance(0.21);
    EXPECT_FALSE(g.paused("BUY"));
}

TEST(SweepGuard, RejectsBadSettings) {
    EXPECT_THROW(SweepGuard(0.0, 0.00003, 0.2), std::invalid_argument);
    EXPECT_THROW(SweepGuard(0.3, 0.0, 0.2), std::invalid_argument);
    EXPECT_THROW(SweepGuard(0.3, 0.00003, -1.0), std::invalid_argument);
}

// ------------------------------------------------------------------ test_drift.py

TEST(DriftWatch, FindsTheSideAMoveWentAgainstAndHoldsIt) {
    FakeClock clock(0.0);
    DriftWatch watch(10.0, 0.00002, 2.0, clock.fn());
    EXPECT_TRUE(watch.observe(100'000.0).empty());
    EXPECT_FALSE(watch.move());  // the window is not covered yet
    clock.advance(10.0);
    EXPECT_TRUE(watch.observe(100'000.0).empty());
    EXPECT_EQ(watch.move(), 0.0);
    clock.advance(1.0);
    EXPECT_EQ(watch.observe(99'997.0), (std::vector<std::string> {"BUY"}));  // -0.3 bp in 10 s: against buys
    EXPECT_TRUE(watch.against("BUY") && !watch.against("SELL"));
    EXPECT_TRUE(approx(*watch.for_side("BUY"), -0.00003));
    EXPECT_TRUE(approx(*watch.for_side("SELL"), 0.00003));
    clock.advance(1.0);
    EXPECT_TRUE(watch.observe(99'998.5).empty());  // -0.15 bp: under the threshold, but the hold runs on
    EXPECT_TRUE(watch.against("BUY"));
    clock.advance(1.5);
    EXPECT_FALSE(watch.against("BUY"));  // 2 s past the last reading that far down
    EXPECT_EQ(watch.episodes, counts(1, 0));
    EXPECT_TRUE(approx(watch.seconds("BUY"), 2.0));
    clock.advance(10.0);
    EXPECT_EQ(watch.observe(100'002.0), (std::vector<std::string> {"SELL"}));  // +0.35 bp over the 99998.5 of 10 s ago
}

TEST(DriftWatch, RejectsBadSettings) {
    EXPECT_THROW(DriftWatch(0.0, 0.00002, 2.0), std::invalid_argument);
    EXPECT_THROW(DriftWatch(10.0, 0.0, 2.0), std::invalid_argument);
    EXPECT_THROW(DriftWatch(10.0, 0.00002, -1.0), std::invalid_argument);
}

// -------------------------------------------------------------------- test_lag.py

TEST(LagGuard, ALateReadingPausesBothSidesAndLaterOnesExtendIt) {
    FakeClock clock;
    LagGuard guard(200.0, 0.5, clock.fn());
    EXPECT_FALSE(guard.observe(199.9));
    EXPECT_FALSE(guard.paused("BUY"));
    auto trips = guard.observe(350.0);
    ASSERT_TRUE(trips);
    auto [buy, sell] = *trips;
    EXPECT_EQ(buy.side, "BUY");
    EXPECT_EQ(sell.side, "SELL");
    EXPECT_TRUE(buy.fresh && sell.fresh && buy.mean == 200.0);
    EXPECT_TRUE(guard.paused("BUY") && guard.paused("SELL"));
    EXPECT_EQ(guard.worst, 350.0);
    clock.advance(0.3);
    trips = guard.observe(900.0);  // still late: the pause runs on, from this reading
    ASSERT_TRUE(trips);
    EXPECT_FALSE(trips->first.fresh);
    EXPECT_EQ(guard.extended, counts(1, 1));
    EXPECT_EQ(guard.worst, 900.0);
    clock.advance(0.4);
    EXPECT_TRUE(guard.paused("BUY"));
    EXPECT_FALSE(guard.observe(120.0));  // on time again, but the pause holds
    clock.advance(0.11);
    EXPECT_FALSE(guard.paused("BUY") || guard.paused("SELL"));
    EXPECT_TRUE(approx(guard.paused_for(), 0.8));  // 1000.0 .. 1000.8
    clock.advance(1.0);
    trips = guard.observe(250.0);  // a new pause: the last one is kept for its log line
    ASSERT_TRUE(trips);
    EXPECT_TRUE(trips->first.fresh);
    EXPECT_EQ(guard.trips, counts(2, 2));
    ASSERT_TRUE(guard.previous["BUY"]);
    auto [since, until, extended] = *guard.previous["BUY"];
    EXPECT_TRUE(approx(since, 1000.0));
    EXPECT_TRUE(approx(until, 1000.8));
    EXPECT_EQ(extended, 1);
    EXPECT_EQ(guard.worst, 250.0);
    EXPECT_EQ(guard.previous_worst, 900.0);
    clock.advance(0.2);
    EXPECT_TRUE(approx(guard.paused_for(), 1.0));  // 0.8 over + 0.2 running
    EXPECT_THROW(LagGuard(0.0, 0.5), std::invalid_argument);
    EXPECT_THROW(LagGuard(200.0, -1.0), std::invalid_argument);
}

// -------------------------------------------------------------- test_take_guard.py

TEST(TakeGuard, ATakePausesTheSideItHitAndMoreTakesExtendIt) {
    FakeClock clock(0.0);
    TakeGuard g(300.0, 1.0, clock.fn());
    EXPECT_FALSE(g.observe("BUY", 299.0));  // under min_usd
    auto trip = g.observe("BUY", 500.0);  // a taker buy lifts the asks: our sell
    ASSERT_TRUE(trip);
    EXPECT_EQ(trip->side, "SELL");
    EXPECT_TRUE(trip->fresh);
    EXPECT_EQ(trip->mean, 500.0);
    EXPECT_TRUE(g.paused("SELL") && !g.paused("BUY"));
    clock.advance(0.6);
    EXPECT_FALSE(g.observe("BUY", 1000.0)->fresh);  // extends it to 1.6
    clock.advance(0.9);
    EXPECT_TRUE(g.paused("SELL"));
    clock.advance(0.2);
    EXPECT_FALSE(g.paused("SELL"));
    EXPECT_EQ(g.observe("SELL", 300.0)->side, "BUY");
    EXPECT_THROW(TakeGuard(-1.0, 1.0), std::invalid_argument);
}

namespace {
// `n` takes of `taker` with the Arcus mid at `before` as each is read and `after` a second later.
std::vector<std::optional<nmm::Trip>> score(TakeGuard& g, const FakeClock& clock, const std::string& taker,
                                            const std::string& side, double before, double after, int n = 1) {
    std::vector<std::optional<nmm::Trip>> trips;
    for (int i = 0; i < n; ++i) {
        g.mark(before);
        clock.advance(0.1);
        trips.push_back(g.observe(side, 500.0, taker));
        clock.advance(0.2);
        g.mark(after);
        clock.advance(0.9);
        g.mark(after);  // 1.1 s after the take: scored at the mid that stood 1 s after it
        clock.advance(2.0);  // the pause is over
    }
    return trips;
}
}  // namespace

TEST(TakeGuard, ATakerThePriceDoesNotFollowTurnsQuietAndBack) {
    FakeClock clock(0.0);
    TakeGuard g(0.0, 1.0, clock.fn(), 3, 0.00002, 1.0);
    EXPECT_TRUE(g.scoring());
    // a TWAP seller: the mid stays put after its takes; its first 3 still trip, then it is quiet
    auto trips = score(g, clock, "0xtwap", "SELL", 100.0, 100.0, 5);
    std::vector<bool> tripped;
    for (auto& t : trips) tripped.push_back(t.has_value());
    EXPECT_EQ(tripped, (std::vector<bool> {true, true, true, false, false}));
    EXPECT_EQ(g.quiet, (std::set<std::string> {"0xtwap"}));
    EXPECT_EQ(g.skipped, 2);
    // an informed buyer: +1 bp its way a second later, never quiet
    trips = score(g, clock, "0xinformed", "BUY", 100.0, 100.01, 4);
    for (auto& t : trips) {
        ASSERT_TRUE(t);
        EXPECT_EQ(t->side, "SELL");
    }
    EXPECT_FALSE(g.quiet.count("0xinformed"));
    // the quiet one starts to move the price (a sell, the mid down 3 bp): back once its mean is over 0.2 bp
    std::vector<std::tuple<std::string, int, double, bool>> flips;
    for (int i = 0; i < 3; ++i) {
        g.mark(100.0);
        clock.advance(0.1);
        g.observe("SELL", 500.0, "0xtwap");
        clock.advance(0.2);
        g.mark(99.97);
        clock.advance(0.9);
        for (auto& f : g.mark(99.97)) flips.push_back(f);
        clock.advance(2.0);
    }
    ASSERT_EQ(flips.size(), 1u);
    EXPECT_EQ(std::get<0>(flips[0]), "0xtwap");
    EXPECT_FALSE(std::get<3>(flips[0]));
    EXPECT_FALSE(g.quiet.count("0xtwap"));
    EXPECT_TRUE(g.observe("SELL", 500.0, "0xtwap"));
}

TEST(TakeGuard, ATakeIsScoredFromTheMidBeforeIt) {
    // the book update of the match read just before its trades: the score starts from the mid before that
    FakeClock clock(0.0);
    TakeGuard g(0.0, 1.0, clock.fn(), 1, 0.00002, 1.0);
    g.mark(100.0);
    clock.advance(0.5);
    g.mark(99.99);  // the take's own update, 2 ms before its trades
    clock.advance(0.002);
    g.observe("SELL", 500.0, "0xa");
    clock.advance(1.0);
    EXPECT_TRUE(g.mark(99.99).empty());  // 1 bp its way from 100.00: informed, not quiet
    EXPECT_FALSE(g.quiet.count("0xa"));
    // without a mid before it (no book yet), a take is not scored, but it pauses
    FakeClock clock2(0.0);
    TakeGuard g2(0.0, 1.0, clock2.fn(), 1);
    EXPECT_TRUE(g2.observe("BUY", 500.0, "0xb"));
    EXPECT_TRUE(g2.pending.empty());
}

TEST(TakeGuard, WithoutMinTakesEveryTakeCounts) {
    FakeClock clock(0.0);
    TakeGuard g(0.0, 1.0, clock.fn());
    EXPECT_FALSE(g.scoring());
    for (auto& t : score(g, clock, "0xtwap", "SELL", 100.0, 100.0, 30)) EXPECT_TRUE(t);
    EXPECT_TRUE(g.quiet.empty());
    EXPECT_TRUE(g.pending.empty());
    EXPECT_THROW(TakeGuard(0.0, 1.0, nmm::monotonic, 101), std::invalid_argument);
}

TEST(TakeGuard, TheLongestUnseenTakerIsForgottenPastTheLimit) {
    FakeClock clock(0.0);
    TakeGuard g(0.0, 1.0, clock.fn(), 1, 0.00002, 1.0);
    score(g, clock, "0xfirst", "SELL", 100.0, 100.0);  // quiet after one take
    EXPECT_TRUE(g.quiet.count("0xfirst"));
    for (int i = 0; i < nmm::TAKER_ADDRESSES; ++i) {
        g.mark(100.0);
        clock.advance(0.1);
        g.observe("BUY", 500.0, "0x" + std::to_string(i));
        clock.advance(1.0);
        g.mark(100.01);
    }
    EXPECT_FALSE(g.quiet.count("0xfirst"));  // forgotten with its scores
}
