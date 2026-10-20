// test_live_19oct.py: the live configs as the owner set them on 19 Oct evening.
#include <gtest/gtest.h>

#include "config.hpp"

using namespace nmm;

namespace {

Config cfg(const std::string& name) { return load_config(std::string(NMM_SOURCE_DIR) + "/config." + name + ".toml"); }

}  // namespace

TEST(Live19Oct, BtcMlLiveTheOkxAndFuturesGuardsTheTakePauseAndTheDriftMarkup) {
    Config c = cfg("btc-ml-okx-only");
    const TradingConfig& t = c.trading;
    EXPECT_EQ(c.pricing.model, "ml");
    EXPECT_EQ(t.direction, "both");
    EXPECT_EQ(t.markup, 0.00002);
    EXPECT_EQ(t.order_size, 0.0122);
    EXPECT_EQ(t.max_position_usd, 2500.0);
    EXPECT_FALSE(t.never_lead);
    EXPECT_TRUE(950 < t.order_size * 82'000 && t.order_size * 82'000 < 1050);  // ~$1000
    EXPECT_TRUE(c.okx.enabled);
    EXPECT_EQ(c.okx.inst_id, "BTC-USDT-SWAP");
    EXPECT_EQ(c.okx.window_ms, 200.0);
    EXPECT_EQ(c.okx.drawdown, 0.00002);
    EXPECT_EQ(c.okx.pause_ms, 300.0);
    EXPECT_EQ(c.futures.window_ms, 100.0);
    EXPECT_EQ(c.futures.drawdown, 0.00002);
    EXPECT_EQ(c.futures.pause_ms, 300.0);
    EXPECT_EQ(c.takes.pause_ms, 300.0);
    EXPECT_EQ(c.drift.markup_factor, 2.0);
    for (double off : {c.guard.drawdown, c.trades.drawdown, c.bybit.drawdown, c.arcus_guard.drawdown, c.trend.drawdown,
                       c.add_guard.drawdown, c.lag.max_ms})
        EXPECT_EQ(off, 0);
}

TEST(Live19Oct, EthMlLiveTheOkxGuardTheTakePauseAndTheDriftMarkupOnly) {
    Config c = cfg("eth-ml-both");
    const TradingConfig& t = c.trading;
    EXPECT_EQ(t.markup, 0.000015);
    EXPECT_EQ(t.order_size, 0.414);
    EXPECT_EQ(t.max_position_usd, 2500.0);
    EXPECT_FALSE(t.never_lead);
    EXPECT_TRUE(950 < t.order_size * 2'415 && t.order_size * 2'415 < 1050);
    EXPECT_TRUE(c.okx.enabled);
    EXPECT_EQ(c.okx.drawdown, 0.00002);
    EXPECT_EQ(c.takes.pause_ms, 300.0);
    EXPECT_EQ(c.drift.markup_factor, 2.0);
    for (double off : {c.guard.drawdown, c.futures.drawdown, c.trades.drawdown, c.bybit.drawdown, c.arcus_guard.drawdown,
                       c.trend.drawdown, c.add_guard.drawdown, c.lag.max_ms})
        EXPECT_EQ(off, 0);
    EXPECT_TRUE(c.futures.enabled && c.bybit.enabled);  // the feeds stay: pricing, drift and the model read them
}

TEST(Live19Oct, GoldOrdersOf500) {
    const TradingConfig t = cfg("gld-both").trading;
    EXPECT_EQ(t.order_size, 1.31);
    EXPECT_TRUE(480 < t.order_size * 380 && t.order_size * 380 < 520);
    EXPECT_EQ(t.max_position_usd, 3000.0);
}

TEST(Live19Oct, NvdaTolAwayHalfABp) {
    for (const char* name : {"nvda-both", "nvda"}) EXPECT_EQ(cfg(name).trading.tol_away, 0.00005) << name;
}

TEST(Live19Oct, TheLiveCryptoBotsWeighTheClassLeanAgainst2500) {
    for (const char* name : {"btc-ml-okx-only", "eth-ml-both"}) {
        const PortfolioConfig pf = cfg(name).portfolio;
        EXPECT_TRUE(pf.enabled) << name;
        EXPECT_EQ(pf.asset_class, "crypto") << name;
        EXPECT_EQ(pf.limit_usd, 2500.0) << name;
        EXPECT_EQ(pf.factor, 0.3) << name;
    }
}

// C++ checkout: config.btc-ml-both.toml is the live BTC ML set, config.btc-ml-okx-only.toml as it is but for the
// markup (0.1 bp from 20 Oct), for the C++ bot live next to the Python bots on one host.
TEST(Live19Oct, TheCppBtcMlBothConfigIsTheLiveOkxOnlySet) {
    Config cpp = cfg("btc-ml-both");
    EXPECT_EQ(cpp.trading.markup, 0.00001);
    cpp.trading.markup = cfg("btc-ml-okx-only").trading.markup;
    EXPECT_EQ(describe(cpp), describe(cfg("btc-ml-okx-only")));
}

// C++ checkout: config.spy-both.toml, a base paper config for SPY-USD with the book collector's feeds of 19 Oct.
TEST(SpyConfig, TheSpyPaperConfigReadsTheSpyFeeds) {
    Config c = cfg("spy-both");
    EXPECT_EQ(c.arcus.market, "SPY-USD");
    EXPECT_EQ(c.binance.symbol, "SPYBUSDT");
    EXPECT_EQ(strings(c.arcus.feed_assets), (std::vector<std::string> {"SPYB"}));
    EXPECT_TRUE(c.futures.enabled);
    EXPECT_EQ(c.futures.symbol, "SPYUSDT");
    EXPECT_EQ(c.pricing.reference, "futures");
    EXPECT_TRUE(c.okx.enabled);
    EXPECT_EQ(c.okx.inst_id, "SPY-USDT-SWAP");
    EXPECT_EQ(c.okx.drawdown, 0);  // a feed, no guard
    EXPECT_FALSE(c.bybit.enabled);
    EXPECT_EQ(c.trading.direction, "both");
    EXPECT_TRUE(900 < c.trading.order_size * 768 && c.trading.order_size * 768 < 1100);  // ~$1000
    EXPECT_FALSE(c.portfolio.enabled);
}
