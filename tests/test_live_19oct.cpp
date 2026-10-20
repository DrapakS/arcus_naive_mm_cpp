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
    EXPECT_EQ(t.order_size, 0.627);
    EXPECT_EQ(t.max_position_usd, 4000.0);
    EXPECT_FALSE(t.never_lead);
    EXPECT_TRUE(1450 < t.order_size * 2'393 && t.order_size * 2'393 < 1550);  // ~$1500 (19 Oct night)
    EXPECT_TRUE(2 * 0.01 / 2'400 < t.tol && t.tol < 2.2 * 0.01 / 2'400);  // 2 ticks (20 Oct)
    EXPECT_TRUE(c.okx.enabled);
    EXPECT_EQ(c.okx.drawdown, 0.00002);
    EXPECT_EQ(c.takes.pause_ms, 300.0);
    EXPECT_EQ(c.drift.markup_factor, 2.0);
    for (double off : {c.guard.drawdown, c.futures.drawdown, c.trades.drawdown, c.bybit.drawdown, c.arcus_guard.drawdown,
                       c.trend.drawdown, c.add_guard.drawdown})
        EXPECT_EQ(off, 0);
    EXPECT_EQ(c.lag.max_ms, 200.0);  // the late-data pause ON (20 Oct)
    EXPECT_EQ(c.lag.pause_ms, 500.0);
    EXPECT_EQ(c.lag.bybit_max_ms, 0.0);
    EXPECT_TRUE(c.futures.enabled && c.bybit.enabled);  // the feeds stay: pricing, drift and the model read them
}

TEST(Live19Oct, GoldOrdersOf500) {
    const TradingConfig t = cfg("gld-both").trading;
    EXPECT_EQ(t.order_size, 2.63);
    EXPECT_TRUE(980 < t.order_size * 380 && t.order_size * 380 < 1020);
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
    EXPECT_EQ(cpp.trading.order_size, 0.0244);  // x2 (20 Oct)
    EXPECT_EQ(cpp.trading.max_position_usd, 3750.0);  // x1.5 (20 Oct)
    Config live = cfg("btc-ml-okx-only");
    cpp.trading.markup = live.trading.markup;
    cpp.trading.order_size = live.trading.order_size;
    cpp.trading.max_position_usd = live.trading.max_position_usd;
    EXPECT_EQ(describe(cpp), describe(cfg("btc-ml-okx-only")));
}

// C++ checkout: config.spy-both.toml, a base paper config for SPY-USD: the Binance SPYUSDT perp its only feed.
TEST(SpyConfig, TheSpyPaperConfigReadsTheBinancePerpOnly) {
    Config c = cfg("spy-both");
    EXPECT_EQ(c.arcus.market, "SPY-USD");
    EXPECT_TRUE(c.futures.enabled);
    EXPECT_EQ(c.futures.symbol, "SPYUSDT");
    EXPECT_EQ(c.pricing.reference, "futures");
    EXPECT_GT(c.futures.drawdown, 0);  // the guard is the perp's
    EXPECT_EQ(c.binance.symbol, "SPYBUSDT");  // read (no switch for it), nothing else
    EXPECT_EQ(strings(c.arcus.feed_assets), (std::vector<std::string> {"SPYB"}));
    EXPECT_EQ(c.guard.drawdown, 0);
    EXPECT_FALSE(c.trades.enabled);
    EXPECT_FALSE(c.okx.enabled);
    EXPECT_FALSE(c.bybit.enabled);
    EXPECT_EQ(c.trading.direction, "both");
    EXPECT_TRUE(190 < c.trading.order_size * 768 && c.trading.order_size * 768 < 210);  // ~$200
    EXPECT_EQ(c.trading.max_position_usd, 500.0);
    EXPECT_FALSE(c.portfolio.enabled);
}

// C++ checkout: config.qqq-both.toml, config.spy-both.toml with QQQ's market and feeds.
TEST(SpyConfig, TheQqqConfigIsTheSpyOneOnQqq) {
    Config q = cfg("qqq-both"), s = cfg("spy-both");
    EXPECT_EQ(q.arcus.market, "QQQ-USD");
    EXPECT_EQ(q.futures.symbol, "QQQUSDT");
    EXPECT_EQ(q.binance.symbol, "QQQBUSDT");
    EXPECT_EQ(strings(q.arcus.feed_assets), (std::vector<std::string> {"QQQB"}));
    EXPECT_TRUE(190 < q.trading.order_size * 715 && q.trading.order_size * 715 < 210);  // ~$200
    q.arcus.market = s.arcus.market;  // all else as SPY's
    q.arcus.feed_assets = s.arcus.feed_assets;
    q.futures.symbol = s.futures.symbol;
    q.binance.symbol = s.binance.symbol;
    q.trading.order_size = s.trading.order_size;
    EXPECT_EQ(describe(q), describe(s));
}

// C++ checkout: config.sol-basic.toml, config.sol-both.toml with every guard on (ETH's Bybit, OKX and drift ones,
// over one tick of SOL's perps).
TEST(SolConfig, TheSolBasicConfigIsSolBothWithEveryGuardOn) {
    Config b = cfg("sol-basic"), s = cfg("sol-both");
    EXPECT_EQ(b.arcus.market, "SOL-USD");
    EXPECT_TRUE(b.bybit.enabled);
    EXPECT_EQ(b.bybit.symbol, "SOLUSDT");
    EXPECT_EQ(b.bybit.drawdown, 0.00015);
    EXPECT_TRUE(b.okx.enabled);
    EXPECT_EQ(b.okx.inst_id, "SOL-USDT-SWAP");
    EXPECT_EQ(b.okx.drawdown, 0.00015);
    EXPECT_EQ(b.drift.markup_factor, 2.5);
    EXPECT_EQ(b.drift.threshold, 0.0001);
    for (double on : {b.guard.drawdown, b.futures.drawdown, b.trades.drawdown, b.arcus_guard.drawdown, b.trend.drawdown,
                      b.add_guard.drawdown, b.lag.max_ms, b.takes.pause_ms})
        EXPECT_GT(on, 0);
    EXPECT_EQ(b.trading.max_position_usd, 1500.0);  // 20 Oct, the owner's (sol-both: $3000)
    b.trading.max_position_usd = s.trading.max_position_usd;
    b.bybit = s.bybit;  // all else as SOL's
    b.okx = s.okx;
    b.drift = s.drift;
    EXPECT_EQ(describe(b), describe(s));
}
