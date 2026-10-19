#include <gtest/gtest.h>

#include <cmath>
#include <iomanip>
#include <random>

#include "pricing.hpp"
#include "test_util.hpp"

using nmm::EwmaPricer;
using nmm::RatioPricer;
using nmm::test::FakeClock;

namespace {

::testing::AssertionResult approx(double actual, double expected, double rel = 1e-6, double abs_ = 1e-12) {
    if (std::fabs(actual - expected) <= std::max(rel * std::fabs(expected), abs_)) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << std::setprecision(17) << actual << " is not approximately " << expected;
}

}  // namespace

TEST(Fsum, IsCorrectlyRounded) {
    EXPECT_EQ(nmm::fsum({0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1}), 1.0);
    EXPECT_EQ(nmm::fsum({1e100, 1.0, -1e100, 1e-100, 1e50, -1.0, -1e50}), 1e-100);
    EXPECT_EQ(nmm::fsum({}), 0.0);
    // the half-way case CPython corrects: 1 + 2**-53 + 2**-106 rounds up
    EXPECT_EQ(nmm::fsum({1.0, std::ldexp(1.0, -53), std::ldexp(1.0, -106)}), 1.0 + std::ldexp(1.0, -52));
}

TEST(RatioPricer, FairPriceIsReferenceTimesMeanRatio) {
    RatioPricer pricer(3, 2);
    pricer.observe(101.0, 100.0);  // 1.01
    EXPECT_FALSE(pricer.ratio());  // warming up
    EXPECT_FALSE(pricer.fair(100.0));
    pricer.observe(204.0, 200.0);  // 1.02
    EXPECT_TRUE(approx(*pricer.ratio(), 1.015));
    EXPECT_TRUE(approx(*pricer.fair(210.0), 213.15));
}

TEST(RatioPricer, WindowKeepsOnlyLastSamples) {
    RatioPricer pricer(3, 1);
    for (double venue : {110.0, 101.0, 102.0, 103.0}) pricer.observe(venue, 100.0);  // 1.10 is evicted by the fourth
    EXPECT_EQ(pricer.samples(), 3u);
    EXPECT_EQ(pricer.count, 4);
    EXPECT_TRUE(approx(*pricer.ratio(), 1.02));
}

TEST(RatioPricer, RunningMeanMatchesExactMeanOverLongRuns) {
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> ref(50'000, 70'000), spread(0.999, 1.001);
    RatioPricer pricer(1000, 1000);
    std::vector<double> ratios;
    for (int i = 0; i < 25'000; ++i) {
        double reference = ref(rng);
        double venue = reference * spread(rng);
        pricer.observe(venue, reference);
        ratios.push_back(venue / reference);
    }
    std::vector<double> last(ratios.end() - 1000, ratios.end());
    EXPECT_TRUE(approx(*pricer.ratio(), nmm::fsum(last) / 1000, 1e-12));
}

TEST(RatioPricer, RatioStdInBps) {
    RatioPricer pricer(4, 2);
    for (double venue : {100.0, 100.02, 100.0, 100.02}) pricer.observe(venue, 100.0);
    // ratios 1.0 / 1.0002 alternate: std = 1 bp of the mean, in bp
    EXPECT_TRUE(approx(*pricer.ratio_std_bps(), 1.0, 1e-3));
}

TEST(RatioPricer, RejectsBadParameters) {
    EXPECT_THROW(RatioPricer(10, 11), std::invalid_argument);
    EXPECT_THROW(RatioPricer(10, 0), std::invalid_argument);
}

namespace {
struct Ewma {
    FakeClock clock;
    EwmaPricer pricer;
    explicit Ewma(double tau = 30.0, int min_samples = 1) : pricer(tau, min_samples, clock.fn()) {}
};
}  // namespace

TEST(EwmaPricer, BasisFollowsAStepWithItsTimeConstant) {
    Ewma e(10.0);
    auto& [clock, pricer] = e;
    pricer.observe(101.0, 100.0);  // ratio 1.01
    clock.advance(100.0);
    pricer.observe(102.0, 100.0);  // a step to 1.02
    EXPECT_TRUE(approx(*pricer.ratio(), 1.01, 0, 1e-6));  // the new ratio has stood for no time yet
    clock.advance(10.0);  // one time constant
    EXPECT_TRUE(approx(*pricer.ratio(), 1.02 - 0.01 * std::exp(-1)));
    EXPECT_TRUE(approx(*pricer.fair(200.0), 200.0 * *pricer.ratio()));
}

TEST(EwmaPricer, WeighsEachRatioByHowLongItStoodNotHowOftenItCame) {
    Ewma burst(10.0);
    burst.pricer.observe(101.0, 100.0);
    burst.clock.advance(10.0);
    for (int i = 0; i < 1000; ++i) burst.pricer.observe(103.0, 100.0);  // a burst at a new ratio, all at once
    EXPECT_TRUE(approx(*burst.pricer.ratio(), 1.01));  // the burst's ratio has stood for no time yet
    Ewma lone(10.0);
    lone.pricer.observe(101.0, 100.0);
    lone.clock.advance(10.0);
    lone.pricer.observe(103.0, 100.0);
    burst.clock.advance(1.0);
    lone.clock.advance(1.0);
    EXPECT_TRUE(approx(*burst.pricer.ratio(), *lone.pricer.ratio()));  // 1000 updates or one: the same basis
}

TEST(EwmaPricer, WarmsUpAndReportsItsSpread) {
    Ewma e(30.0, 3);
    auto& [clock, pricer] = e;
    pricer.observe(101.0, 100.0);
    clock.advance(1.0);
    pricer.observe(101.2, 100.0);
    EXPECT_FALSE(pricer.ratio());
    EXPECT_FALSE(pricer.fair(100.0));
    EXPECT_EQ(pricer.samples(), 2u);
    clock.advance(1.0);
    pricer.observe(100.8, 100.0);
    EXPECT_TRUE(pricer.ratio());
    EXPECT_GT(*pricer.ratio_std_bps(), 0);
    EXPECT_THROW(EwmaPricer(0.0, 1), std::invalid_argument);
}

TEST(EwmaPricer, StartsFromTheMeanOfWhatItHasSeenNotTheFirstRatio) {
    Ewma e(30.0);
    auto& [clock, pricer] = e;
    pricer.observe(101.0, 100.0);  // 1.01 for 0.25 s
    clock.advance(0.25);
    pricer.observe(100.0, 100.0);  // then 1.00 for 0.25 s
    clock.advance(0.25);
    EXPECT_TRUE(approx(*pricer.ratio(), 1.005, 0, 1e-4));  // about the time-weighted mean of the two
}

TEST(EwmaPricer, HoldsStillWhilePaused) {
    Ewma e(10.0);
    auto& [clock, pricer] = e;
    pricer.observe(101.0, 100.0);
    clock.advance(100.0);  // 1.01 is the basis
    pricer.observe(103.0, 100.0);  // a ratio seen as the book breaks
    clock.advance(0.1);
    pricer.pause();
    double held = *pricer.ratio();
    clock.advance(30.0);  // the outage
    EXPECT_TRUE(approx(*pricer.ratio(), held));
    pricer.observe(101.0, 100.0);  // back: 1.03 counted only for the 0.1 s it was seen
    clock.advance(10.0);
    EXPECT_TRUE(approx(*pricer.ratio(), 1.01, 0, 2e-4));
}

TEST(EwmaPricer, WaitsForEnoughWeight) {
    FakeClock clock;
    EwmaPricer pricer(30.0, 1, clock.fn(), 0.1);
    pricer.observe(101.0, 100.0);
    clock.advance(1.0);
    EXPECT_FALSE(pricer.ratio());  // 1 s of a 30 s average: 3% of its weight
    clock.advance(3.0);
    EXPECT_TRUE(approx(*pricer.ratio(), 1.01));  // 4 s: 12.5%
}
