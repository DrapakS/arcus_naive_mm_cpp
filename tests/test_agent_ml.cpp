// Ported from tests/test_ml.py: the agent priced by the ML model (pricing.model = "ml"), on the
// books the test writes and end to end over the fake exchanges, and the ML paper configs.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <cmath>
#include <functional>
#include <set>

#include "agent_helpers.hpp"
#include "fake_server.hpp"
#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

// pytest.approx: within rel of the expected value, or abs.
#define EXPECT_APPROX(actual, expected, rel, abs_)                                     \
    do {                                                                               \
        double _want = (expected);                                                     \
        EXPECT_NEAR((actual), _want, std::max((rel) * std::abs(_want), (abs_)));       \
    } while (0)

namespace {

const std::string LGBM_SMALL = repo("tests/data/lgbm_zero_missing.txt");  // 4 features, named Column_0..3

ConfigTable table(std::initializer_list<std::pair<std::string, double>> values) {
    ConfigTable out;
    for (const auto& [k, v] : values) out.emplace_back(k, ConfigValue::floating(v));
    return out;
}

double number_of(const ConfigTable& t, const std::string& key) {
    for (const auto& [k, v] : t)
        if (k == key) return v.number;
    throw std::out_of_range(key);
}

std::set<std::string> keys(const ConfigTable& t) {
    std::set<std::string> out;
    for (const auto& [k, v] : t) out.insert(k);
    return out;
}

std::set<std::string> feature_names(const ml::Prediction& p) {
    std::set<std::string> out;
    for (const auto& [k, v] : p.features) out.insert(k);
    return out;
}

using SeriesKeys = std::set<std::pair<std::string, std::string>>;
SeriesKeys series_keys(const ml::FeaturePricer& m) {
    SeriesKeys out;
    for (const auto& [k, v] : m.series) out.insert(k);
    return out;
}

using Venues = std::vector<std::string>;

// An agent priced by the ML model, with a fake gateway and a clock the test moves (test_ml._ml_agent).
Bot ml_agent(ConfigTable weights, const std::function<void(MlConfig&)>& set = {}) {
    Config config;
    config.pricing.model = "ml";
    config.ml.weights = std::move(weights);
    config.ml.reuse_ms = 0;  // every ask worked out: the tests move the clock by less than that
    if (set) set(config.ml);
    config.guard.drawdown = config.futures.drawdown = config.bybit.drawdown = config.arcus_guard.drawdown = 0;
    config.trades.drawdown = config.trend.drawdown = config.lag.max_ms = 0;
    config.validate();
    Bot b;
    b.agent = std::make_unique<Agent>(config);
    b.clock.set(0.0);
    b->ml_clock = b.clock.fn();
    b.gw = install(*b.agent, opts(0.0001, 0.0001, b->guards));
    return b;
}

void tick(Agent& agent, OrderBook& book, double bid, double ask) {
    book.apply_snapshot(std::vector<Level> {{bid, 1.0}}, std::vector<Level> {{ask, 1.0}}, book.seq + 1);
    agent.on_book_update(book);
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(AgentMl, TheMlModeComputesTheModelAndNothingOfTheBasisPricing) {
    Bot b = ml_agent(table({{"intercept", -0.0002}, {"binance_futures_mid_r1s", 2.0}}));
    Agent& agent = *b.agent;
    EXPECT_EQ(agent.pricer, nullptr);
    EXPECT_EQ(agent.spot_pricer, nullptr);
    EXPECT_EQ(agent.futures_pricer, nullptr);
    EXPECT_EQ(agent.ml->venues, (Venues {"arcus", "binance_futures"}));
    for (int k = 0; k < 60; ++k) {  // 6 s of Arcus at 99990.0/99990.2 and futures rising 0.1 a frame
        tick(agent, agent.venue_book, 99990.0, 99990.2);
        tick(agent, *agent.futures_book, 100000.0 + 0.1 * k, 100000.2 + 0.1 * k);
        tick(agent, agent.reference_book, 100001.0, 100001.2);  // spot: a book this model does not read
        b.clock.advance(0.1);
    }
    EXPECT_EQ(agent.futures_fair_bid, std::nullopt);
    EXPECT_EQ(agent.spot_fair_bid, std::nullopt);
    EXPECT_EQ(agent.fair_bid, std::nullopt);
    EXPECT_FALSE(agent.ml->series.contains({"binance", "mid"}));
    auto view = agent.build_view();
    auto pred = A::ml_prediction(agent);
    ASSERT_TRUE(pred.has_value());
    EXPECT_EQ(view->fair_bid, pred->fair_bid);
    ASSERT_TRUE(view->model_bps.has_value());
    EXPECT_APPROX(*view->model_bps, pred->value * 1e4, 1e-6, 1e-12);
    // the futures mid over the last 1 s: 100005.1 .. 100006.0, 0.1 s each, a mean 0.45 under the latest
    double x = pred->feature("binance_futures_mid_r1s");
    EXPECT_APPROX(x, 0.45 / 100005.55, 1e-6, 1e-12);
    EXPECT_APPROX(pred->value, -0.0002 + 2 * x, 1e-6, 1e-12);
    EXPECT_APPROX(pred->fair_bid, 99990.0 + 99990.1 * pred->value, 1e-6, 1e-12);  // the mean bid + P_prev y
    agent.strategy->evaluate(*view);
    ASSERT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}}));
    const Order& order = *b.gw->sent[0].second;
    EXPECT_EQ(order.price, agent.strategy->price("BUY", *view));  // fair bid x (1 - markup), down to the tick
    double under = pred->fair_bid * (1 - 0.0001) - order.price.to_double();
    EXPECT_TRUE(0 <= under && under < 0.1) << under;
    EXPECT_LT(order.price, Decimal("99990"));
    std::string summary = A::pricing_summary(agent);
    EXPECT_TRUE(has(summary, "ml fair bid") && has(summary, "bp over the mid now")) << summary;
}

TEST(AgentMl, TheMlAgentPricesOffTheChangeAndBasisFeatures) {
    Bot b = ml_agent(table({{"binance_futures_mid_d1s", 1.0}, {"basis_binance_futures_mid_ema1s", 1.0}}),
                     [](MlConfig& m) { m.target_window_s = 1.0; });
    Agent& agent = *b.agent;
    EXPECT_EQ(agent.ml->venues, (Venues {"arcus", "binance_futures"}));
    EXPECT_EQ(series_keys(*agent.ml), (SeriesKeys {{"arcus", "bid"}, {"arcus", "mid"}, {"binance_futures", "mid"}}));
    for (int k = 0; k < 40; ++k) {  // 4 s of Arcus standing still and futures rising 0.1 a frame
        tick(agent, agent.venue_book, 99990.0, 99990.2);
        tick(agent, *agent.futures_book, 100000.0 + 0.1 * k, 100000.2 + 0.1 * k);
        b.clock.advance(0.1);
    }
    auto pred = A::ml_prediction(agent);
    ASSERT_TRUE(pred.has_value());
    double d1s = pred->feature("binance_futures_mid_d1s"), basis = pred->feature("basis_binance_futures_mid_ema1s");
    EXPECT_APPROX(d1s, 0.45 / 100003.55, 1e-6, 1e-12);  // Arcus did not move: futures' r1s
    EXPECT_GT(basis, 0);  // futures went up, Arcus did not: Arcus under its usual basis
    EXPECT_APPROX(pred->value, d1s + basis, 1e-6, 1e-12);
    EXPECT_EQ(agent.build_view()->fair_bid, pred->fair_bid);
}

TEST(AgentMl, TheMlPriceIsPulledWhileABookItReadsIsDown) {
    Bot b = ml_agent(table({{"arcus_mid_r1s", 1.0}}));
    Agent& agent = *b.agent;
    for (int k = 0; k < 60; ++k) {
        tick(agent, agent.venue_book, 99990.0, 99990.2);
        b.clock.advance(0.1);
    }
    EXPECT_TRUE(agent.build_view()->fair_bid.has_value());
    agent.venue_book.invalidate();
    agent.on_book_update(agent.venue_book);  // what a feed does when every link is down
    EXPECT_EQ(agent.build_view()->fair_bid, std::nullopt);
    EXPECT_EQ(agent.top_view().fair_bid, std::nullopt);
    EXPECT_EQ(A::pricing_summary(agent), "ml fair bid: n/a (arcus book not ready)");
}

TEST(AgentMl, TheBasisModeBuildsNoModel) {
    Agent agent {Config()};
    EXPECT_EQ(agent.ml, nullptr);
    EXPECT_NE(agent.pricer, nullptr);
    EXPECT_EQ(agent.build_view()->model_bps, std::nullopt);
}

// The whole path on local fake exchanges: the feeds build the books, every frame goes to the
// model, the views carry its price. Windows of 50-100 ms, as the fake streams last under a second.
class AgentMlEndToEnd : public ::testing::TestWithParam<std::string> {};

TEST_P(AgentMlEndToEnd, TheMlPricingRunsOverTheFeeds) {
    const std::string kind = GetParam();
    LogCapture log("naive_mm.agent", logging::WARNING);
    Loop loop;
    FakeBinance binance(loop.ioc, {.n_steps = 400});
    FakeArcus arcus(loop.ioc, {.n_events = 400});
    FakeBinanceFutures futures(loop.ioc, {.n_tickers = 400});
    binance.start();
    arcus.start();
    futures.start();
    Config config;
    config.binance.ws_url = binance.ws_url();
    config.binance.rest_url = binance.rest_url();
    config.arcus.ws_url = arcus.ws_url();
    config.arcus.rest_url = arcus.rest_url();
    config.futures.ws_url = futures.ws_url();
    config.bybit.enabled = config.trades.enabled = false;
    config.pricing.model = "ml";
    if (kind == "linear") {
        config.ml.weights = table({{"intercept", 0.0}, {"arcus_mid_r0.1s", 0.5}, {"binance_futures_mid_r0.1s", 1.0}, {"binance_mid_r0.05s", 0.2}});
    } else {  // the small test model: 4 columns, read here as these 4 features
        config.ml.kind = "lightgbm";
        config.ml.lightgbm_model = LGBM_SMALL;
        for (const char* n : {"arcus_mid_r0.1s", "binance_futures_mid_r0.1s", "binance_mid_r0.05s", "binance_futures_mid_d0.1s"})
            config.ml.features.push_back(ConfigValue::string(n));
    }
    config.ml.target_window_s = 0.1;
    config.ml.eval_interval_ms = 10;
    config.validate();
    Agent agent(config);
    bool finished = false;
    boost::asio::co_spawn(loop.ioc, agent.run(), [&](std::exception_ptr) { finished = true; });
    ASSERT_TRUE(run_until(loop.ioc, [&] { return agent.view && agent.view->fair_bid.has_value(); }, 10));
    auto view = agent.view;
    EXPECT_TRUE(view->model_bps.has_value());
    EXPECT_EQ(view->futures_fair_bid, std::nullopt);
    EXPECT_EQ(view->spot_fair_bid, std::nullopt);
    if (kind == "linear") {
        ASSERT_TRUE(view->venue && view->venue->best_bid());
        double best_bid = *view->venue->best_bid();
        EXPECT_NEAR(*view->fair_bid, best_bid, 0.01 * best_bid);  // the mean bid, moved by a tiny y
    } else {  // the test model was fit on values of ~1: on raw features it says so at start, and prices nonsense
        EXPECT_TRUE(any_starts(log.messages(), "the LightGBM model splits at values of"));
    }
    EXPECT_EQ(agent.pricer, nullptr);
    EXPECT_EQ(agent.futures_fair_bid, std::nullopt);
    EXPECT_EQ(agent.spot_fair_bid, std::nullopt);
    EXPECT_EQ(series_keys(*agent.ml),
              (SeriesKeys {{"arcus", "bid"}, {"arcus", "mid"}, {"binance", "mid"}, {"binance_futures", "mid"}}));
    if (kind == "linear") EXPECT_NE(dynamic_cast<ml::LinearPricer*>(agent.ml.get()), nullptr);
    else EXPECT_NE(dynamic_cast<ml::LightGBMPricer*>(agent.ml.get()), nullptr);
    agent.stop();
    EXPECT_TRUE(run_until(loop.ioc, [&] { return finished; }, 5));
}

INSTANTIATE_TEST_SUITE_P(Kinds, AgentMlEndToEnd, ::testing::Values("linear", "lightgbm"));

// config.btc-ml-both.toml (19 Oct evening): off futures, 21 features with the Bybit perp's,
// skip_missing. The spot and Bybit books never ready hold their features (here at their means: never
// known), the price comes from Arcus, futures and the basis bot's prediction; a log line names what
// is held. Bybit is on for the model only.
TEST(AgentMl, TheMlPaperConfigPricesOnWhenTheSpotAndBybitBooksGoQuiet) {
    LogCapture log(ORDERS_LOGGER);
    Config c = load_config(repo("config.btc-ml-both.toml"));
    EXPECT_EQ(c.ml.target_base, "futures");
    EXPECT_EQ(c.ml.target_window_s, 30.0);
    EXPECT_TRUE(c.ml.skip_missing);
    EXPECT_EQ(strings(c.ml.required), (std::vector<std::string> {"basis_pred_ema30s"}));
    EXPECT_TRUE(c.bybit.enabled);  // a feed, not a guard
    EXPECT_EQ(c.bybit.symbol, "BTCUSDT");
    EXPECT_FALSE(c.bybit.drawdown);
    EXPECT_EQ(c.trading.tol, 0.000005);
    const ConfigTable &w = c.ml.weights, &mean = c.ml.mean, &scale = c.ml.scale;
    std::vector<std::string> names;
    std::set<std::string> name_set, mean_set, scale_set;
    for (const auto& [n, v] : numbers(w))
        if (n != "intercept") {
            names.push_back(n);
            name_set.insert(n);
            EXPECT_NE(v, 0.0) << n;
        }
    for (const auto& [n, v] : numbers(mean)) mean_set.insert(n);
    for (const auto& [n, v] : numbers(scale)) scale_set.insert(n);
    EXPECT_EQ(names.size(), 21u);
    EXPECT_EQ(name_set, mean_set);
    EXPECT_EQ(name_set, scale_set);
    Bot b = ml_agent(c.ml.weights, [&](MlConfig& m) {
        m.target_base = "futures";
        m.target_window_s = 30.0;
        m.skip_missing = true;
        m.required = {ConfigValue::string("basis_pred_ema30s")};
        m.mean = c.ml.mean;
        m.scale = c.ml.scale;
        m.scaled = true;
    });
    Agent& agent = *b.agent;
    EXPECT_EQ(std::set<std::string>(agent.ml->needed_venues.begin(), agent.ml->needed_venues.end()),
              (std::set<std::string> {"arcus", "binance_futures"}));
    EXPECT_EQ(std::set<std::string>(agent.ml->venues.begin(), agent.ml->venues.end()),
              (std::set<std::string> {"arcus", "binance", "binance_futures", "bybit"}));
    for (int k = 0; k < 1000; ++k) {  // 100 s: the 30 s means warm up (90 s)
        b.clock.advance(0.1);
        tick(agent, agent.venue_book, 81'000.0, 81'000.2);
        tick(agent, *agent.futures_book, 81'120.0, 81'120.1);
    }
    auto pred = A::ml_prediction(agent);
    ASSERT_TRUE(pred.has_value());
    std::set<std::string> quiet;
    for (const auto& n : names)
        if (starts_with(n, "binance_imb") || starts_with(n, "binance_mid") || starts_with(n, "bybit_") || starts_with(n, "basis_bybit_"))
            quiet.insert(n);
    EXPECT_EQ(std::set<std::string>(pred->skipped.begin(), pred->skipped.end()), quiet);
    auto messages = log.messages();
    EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& m) {
        return has(m, "--- ml: ") && has(m, "unknown now: held at the last value");
    }));
    // basis_pred is the Arcus mid over the futures mid (-14.8 bp); the held ones count at their means (nothing)
    EXPECT_NEAR(pred->feature("basis_pred_ema30s"), 81'000.1 / 81'120.05 - 1, 1e-9);
    double y = number_of(w, "intercept");
    for (const auto& n : names) {
        double x = number_of(mean, n);
        for (const auto& [f, v] : pred->features)
            if (f == n) x = v;
        y += number_of(w, n) * (x - number_of(mean, n)) / number_of(scale, n);
    }
    EXPECT_NEAR(pred->value, y, 1e-9);
    EXPECT_APPROX(pred->fair_bid, 81'120.05 * (1 + y) - 0.1, 1e-6, 1e-12);
}

// config.eth-ml-both.toml (20 Oct refit, 10 s target): config.eth-both.toml priced by the owner's ETH model, 26 features
// with the Bybit perp's. With the Arcus, spot, futures and Bybit books all live every feature is known
// (none held), and the prediction is the scaled linear formula of the features the model saw.
TEST(AgentMl, TheEthMlPaperConfigKnowsAllIts26FeaturesWhenEveryBookRuns) {
    Config c = load_config(repo("config.eth-ml-both.toml"));
    EXPECT_EQ(c.pricing.model, "ml");
    EXPECT_EQ(c.arcus.market, "ETH-USD");
    EXPECT_TRUE(c.bybit.enabled && !c.bybit.drawdown);  // Bybit: the model's feed, no guard (19 Oct evening)
    EXPECT_TRUE(c.okx.enabled && c.okx.drawdown);  // OKX: a guard only, not the model
    EXPECT_EQ(c.ml.target_base, "futures");
    EXPECT_EQ(c.ml.target_window_s, 10.0);
    EXPECT_TRUE(c.ml.skip_missing);
    EXPECT_EQ(strings(c.ml.required), (std::vector<std::string> {"basis_pred_ema30s"}));
    std::set<std::string> names = keys(c.ml.weights);
    names.erase("intercept");
    EXPECT_EQ(names.size(), 26u);
    EXPECT_EQ(names, keys(c.ml.mean));
    EXPECT_EQ(names, keys(c.ml.scale));
    Bot b = ml_agent(c.ml.weights, [&](MlConfig& m) {
        m.target_base = "futures";
        m.target_window_s = 10.0;
        m.skip_missing = true;
        m.required = {ConfigValue::string("basis_pred_ema30s")};
        m.mean = c.ml.mean;
        m.scale = c.ml.scale;
        m.scaled = true;
    });
    Agent& agent = *b.agent;
    using Names = std::set<std::string>;
    EXPECT_EQ(Names(agent.ml->venues.begin(), agent.ml->venues.end()),
              (Names {"arcus", "binance", "binance_futures", "bybit"}));  // spot and Bybit read, not waited for:
    EXPECT_EQ(Names(agent.ml->needed_venues.begin(), agent.ml->needed_venues.end()),
              (Names {"arcus", "binance_futures"}));  // only the required one and the books
    for (int k = 0; k < 1000; ++k) {  // 100 s: the 30 s means warm up (90 s)
        b.clock.advance(0.1);
        tick(agent, agent.venue_book, 2'400.0 + 0.01 * (k % 7), 2'400.05 + 0.01 * (k % 7));
        tick(agent, agent.reference_book, 2'403.0 + 0.02 * (k % 5), 2'403.01 + 0.02 * (k % 5));
        tick(agent, *agent.futures_book, 2'402.0 + 0.001 * k, 2'402.01 + 0.001 * k);
        tick(agent, *agent.bybit_book, 2'401.9 + 0.01 * (k % 3), 2'401.91 + 0.01 * (k % 3));
    }
    auto pred = A::ml_prediction(agent);
    ASSERT_TRUE(pred.has_value());
    EXPECT_TRUE(pred->skipped.empty());
    EXPECT_EQ(feature_names(*pred), names);
    double y = number_of(c.ml.weights, "intercept");
    for (const auto& n : names)
        y += number_of(c.ml.weights, n) * (pred->feature(n) - number_of(c.ml.mean, n)) / number_of(c.ml.scale, n);
    EXPECT_NEAR(pred->value, y, 1e-12);
}

TEST(AgentMl, APredictionIsReusedWithinReuseMsThenWorkedOutAgain) {
    Loop loop;
    Bot b = ml_agent(table({{"intercept", 0.0}, {"binance_futures_mid_r1s", 1.0}}), [](MlConfig& m) { m.reuse_ms = 1.0; });
    Agent& agent = *b.agent;
    for (int k = 0; k < 400; ++k) {
        b.clock.advance(0.01);
        tick(agent, agent.venue_book, 100'000.0, 100'000.5);
        tick(agent, *agent.futures_book, 100'100.0 + k, 100'100.5 + k);
    }
    auto first = A::ml_prediction(agent);
    ASSERT_TRUE(first);
    b.clock.advance(0.0005);
    tick(agent, *agent.futures_book, 100'900.0, 100'900.5);  // within 1 ms: the same prediction
    EXPECT_EQ(A::ml_prediction(agent)->value, first->value);
    b.clock.advance(0.001);
    EXPECT_NE(A::ml_prediction(agent)->value, first->value);  // the futures jump, worked out now
}

// A burst of futures tickers within [ml] reuse_ms, the prediction reused and the Arcus book, the orders and
// the drift as they were: one cancel check, not one a ticker. An Arcus frame is always checked, and so is a
// frame after reuse_ms or after a fill.
TEST(AgentMl, ThePerFrameCancelCheckIsSkippedWhileNothingItReadsChanged) {
    Loop loop;
    Bot b = ml_agent(table({{"intercept", 0.0}, {"binance_futures_mid_r1s", 1.0}, {"binance_futures_imb1", 1e-5}}),
                     [](MlConfig& m) { m.reuse_ms = 1.0; });
    Agent& agent = *b.agent;
    for (int k = 0; k < 400; ++k) {
        b.clock.advance(0.01);
        tick(agent, agent.venue_book, 100'000.0, 100'000.5);
        tick(agent, *agent.futures_book, 100'100.0 + k, 100'100.5 + k);
    }
    evaluate(agent);
    ASSERT_FALSE(b.gw->orders.empty());
    auto futures = [&](double size) {
        OrderBook& f = *agent.futures_book;
        f.apply_snapshot(std::vector<Level> {{100'500.0, size}}, std::vector<Level> {{100'500.5, 1.0}}, f.seq + 1);
        agent.on_book_update(f);
    };
    b.clock.advance(0.01);
    int64_t before = agent.ml_checks_skipped;
    for (int k = 0; k < 20; ++k) {  // 20 tickers 10 us apart: the sizes move the model's inputs, its prediction is reused
        b.clock.advance(0.00001);
        futures(1 + k % 3);
    }
    EXPECT_GE(agent.ml_checks_skipped - before, 18);
    int64_t skipped = agent.ml_checks_skipped;
    tick(agent, agent.venue_book, 100'000.0, 100'000.5);  // an Arcus frame: checked whatever
    EXPECT_EQ(agent.ml_checks_skipped, skipped);
    b.clock.advance(0.0011);
    futures(5);  // past reuse_ms: checked again
    EXPECT_EQ(agent.ml_checks_skipped, skipped);
    b.gw->add_pos(Decimal("0.001"));  // a fill: the position changed, so within reuse_ms it is checked too
    b.clock.advance(0.00001);
    futures(6);
    EXPECT_EQ(agent.ml_checks_skipped, skipped);
}
