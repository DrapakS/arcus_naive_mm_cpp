// Ported from tests/test_volatility.py: the markup scaled by volatility, sqrt(short mean / long mean)
// of the reference's squared 1 s returns, within low..high, x1 while warming up.
#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <stdexcept>

#include "config.hpp"
#include "strategy_helpers.hpp"
#include "trading/volatility.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

std::string repo_file(const std::string& name) { return std::string(NMM_SOURCE_DIR) + "/" + name; }

std::string load_error(const std::vector<std::string>& overrides) {
    try {
        load_config(std::nullopt, overrides);
    } catch (const std::invalid_argument& err) {
        return err.what();
    }
    return "";
}

// Feed a price that moves +-bp each step (alternating), from `start`; returns (now, price).
std::pair<double, double> run(VolScale& v, double start, double seconds, double bp, double price = 100.0, double step = 1.0) {
    double t = start, p = price;
    for (int k = 0; k < static_cast<int>(seconds / step); ++k) {
        t += step;
        p *= std::exp((k % 2 ? 1 : -1) * bp / 1e4);
        v.observe(p, t);
    }
    return {t, p};
}

bool near_one(const Decimal& factor) { return (factor - Decimal(1)).abs() <= D("0.02"); }

}  // namespace

TEST(Volatility, X1WhileWarmingUpThenTheRatioOfNowToTheUsualWithinLowAndHigh) {
    VolScale v(60, 1800, 0.8, 2.0, 300);
    v.observe(100.0, 0.0);
    auto [t, p] = run(v, 0.0, 299, 1.0);
    EXPECT_EQ(v.factor(), Decimal(1));  // warming up
    std::tie(t, p) = run(v, t, 1500, 1.0, p);
    EXPECT_TRUE(near_one(v.factor())) << v.factor();  // moving as usual
    std::tie(t, p) = run(v, t, 120, 3.0, p);  // a burst at 3x the usual
    EXPECT_GT(v.factor(), D("1.8"));
    EXPECT_LE(v.factor(), Decimal(2));
    std::tie(t, p) = run(v, t, 200, 10.0, p);
    EXPECT_EQ(v.factor(), Decimal(2));  // capped (a burst of 10 min would lift the usual: x1.85 after it)
    std::tie(t, p) = run(v, t, 300, 0.01, p);  // quiet
    EXPECT_EQ(v.factor(), D("0.8"));
    EXPECT_EQ(v.note().value_or(""), "volatility x0.80");
    EXPECT_NE(v.status().find("markup x0.80"), std::string::npos) << v.status();
}

TEST(Volatility, SamplesOnceAStepAndAGapCountsPerStep) {
    VolScale v(60, 1800, 0.8, 2.0, 0);
    v.observe(100.0, 0.0);
    v.observe(200.0, 0.5);  // within the step: not a sample
    EXPECT_FALSE(v.short_var);
    v.observe(100.0 * std::exp(4e-4), 4.0);  // one return over 4 steps: 4 bp, (4 bp)^2 / 4 a step
    ASSERT_TRUE(v.short_var);
    double want = 4e-4 * 4e-4 / 4;
    EXPECT_NEAR(*v.short_var, want, want * 1e-6);
    v.observe(0.0, 5.0);  // no price: ignored
    EXPECT_THROW(VolScale(60, 30), std::invalid_argument);
    EXPECT_THROW(VolScale(60, 1800, 1.2), std::invalid_argument);
}

TEST(Volatility, EveryMarkupTakesTheFactor) {
    VolScale v(60, 1800, 0.8, 2.0, 0);
    v.observe(100.0, 0.0);
    run(v, 0.0, 1800, 1.0);
    run(v, 1800.0, 200, 10.0);
    ASSERT_EQ(v.factor(), Decimal(2));
    auto [gw0, plain] = both();
    auto [gw1, scaled] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.vol = &v; });
    plain->evaluate(view(100'000.0));
    scaled->evaluate(view(100'000.0));
    auto prices = [](FakeGateway& gw) {
        std::map<std::string, Decimal> out;
        for (const auto& a : actions(gw)) out[a.side] = a.price;
        return out;
    };
    using Prices = std::map<std::string, Decimal>;
    EXPECT_EQ(prices(*gw0), (Prices {{"BUY", D("99900.0")}, {"SELL", D("100100.2")}}));  // markup 0.001
    EXPECT_EQ(prices(*gw1), (Prices {{"BUY", D("99800.0")}, {"SELL", D("100200.2")}}));  // x2
}

TEST(Volatility, VolMarkupConfigIsCheckedAndOnInTheTradingConfigs) {
    EXPECT_FALSE(Config().vol_markup.enabled);
    std::string err = load_error({"vol_markup.enabled=true", "vol_markup.low=1.5"});
    EXPECT_NE(err.find("vol_markup needs"), std::string::npos) << err;
    for (const std::string name : {"btc-both", "btc-ml-both", "eth-both", "eth-ml-both", "sol-both", "gld-both", "nvda-both",
                                   "recommended", "eth", "sol", "gld", "nvda", "btc-ml"}) {
        const VolMarkupConfig vm = load_config(repo_file("config." + name + ".toml")).vol_markup;
        EXPECT_EQ(std::make_tuple(vm.enabled, vm.short_s, vm.long_s, vm.low, vm.high, vm.warmup_s),
                  std::make_tuple(true, 60.0, 1800.0, 0.8, 2.0, 300.0))
            << name;
    }
}

TEST(Volatility, RightAfterTheWarmUpASteadyMarketIsX1NotTheCap) {
    // The long mean starts from nothing: unweighted, 5 min in it held 15% of the usual and the factor sat at x2.
    VolScale v(60, 1800, 0.8, 2.0, 300);
    v.observe(100.0, 0.0);
    run(v, 0.0, 301, 1.0);
    EXPECT_TRUE(near_one(v.factor())) << v.factor();
}
