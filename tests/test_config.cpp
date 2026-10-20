// The settings the Python tests check outside test_agent.py: [ml] (test_ml.py), [open_guard] and
// [open_halt] (test_opens.py) and the market spec's hours (test_session.py).
#include <gtest/gtest.h>

#include "agent_helpers.hpp"
#include "trading/session.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

const std::string LGBM_SMALL = repo("tests/data/lgbm_zero_missing.txt");  // 4 features, named Column_0..3
const std::vector<std::string> LGBM_NAMES {"arcus_mid_r1s", "binance_mid_d5s", "basis_binance_futures_mid_ema1s",
                                           "binance_futures_mid_r5s"};

const std::string ML_TOML = R"([pricing]
model = "ml"
[ml]
target_window_s = 5
[ml.weights]
intercept = 0.00001
arcus_mid_r1s = -0.25
binance_futures_mid_r5s = 0.5
binance_mid_r10s = 0.1
)";

double number_of(const ConfigTable& table, const std::string& key) {
    for (const auto& [k, v] : table)
        if (k == key) return v.number;
    ADD_FAILURE() << "no " << key;
    return 0.0;
}

using Cases = std::vector<std::pair<std::string, std::string>>;

void expect_refused(TempFile& f, const Cases& cases) {
    for (const auto& [bad, match] : cases) {
        SCOPED_TRACE(bad);
        EXPECT_LOAD_ERROR(f.write(bad), {}, match);
    }
}

std::vector<std::string> reprs(const ConfigList& list) {
    std::vector<std::string> out;
    for (const auto& v : list) out.push_back(v.repr);
    return out;
}

}  // namespace

TEST(MlConfig, MlSettings) {
    TempFile f;
    Config config = load_config(f.write(ML_TOML));
    EXPECT_EQ(config.pricing.model, "ml");
    EXPECT_EQ(number_of(config.ml.weights, "binance_futures_mid_r5s"), 0.5);
    EXPECT_EQ(Config().pricing.model, "basis");
    EXPECT_TRUE(Config().ml.weights.empty());
    expect_refused(f, {
        {"[pricing]\nmodel = \"ml\"\n", "needs the model"},
        {"[pricing]\nmodel = \"neural\"\n", "pricing.model"},
        {"[ml.weights]\nokx_mid_r5s = 1.0\n", "not a feature of the notebook"},  // caught with model = basis too
        {"[ml.weights]\narcus_mid_r1s = \"0.5\"\n", "finite number"},
        {"[ml]\ntarget_price = \"last\"\n", "ml.target_price"},
        {"[ml]\neval_interval_ms = -1\n", "eval_interval_ms"},
        {"[pricing]\nmodel = \"ml\"\n[bybit]\nenabled = false\n[ml.weights]\nbybit_mid_r1s = 1.0\n", "[bybit] enabled"},
        {"[pricing]\nmodel = \"ml\"\n[futures]\nenabled = false\n[ml.weights]\nbinance_futures_mid_r1s = 1.0\n",
         "[futures] enabled"},
    });
    // with the model on, pricing off futures is not asked for: the basis pricing is not used
    f.write("[pricing]\nmodel = \"ml\"\nreference = \"futures\"\n[futures]\nenabled = false\n[ml.weights]\narcus_mid_r1s = 1.0\n");
    EXPECT_EQ(load_config(f.written).pricing.model, "ml");
}

TEST(MlConfig, TheChangeAndBasisFeaturesInTheSettings) {
    TempFile f;
    f.write("[pricing]\nmodel = \"ml\"\n[ml.weights]\nbinance_mid_d5s = 1e-5\nbasis_binance_futures_mid_ema30s = 2e-5\n");
    Config config = load_config(f.written);
    EXPECT_EQ(number_of(config.ml.weights, "basis_binance_futures_mid_ema30s"), 2e-5);
    EXPECT_EQ(Agent(config).ml->warmup_s, 90.0);
    expect_refused(f, {
        {"[ml.weights]\narcus_mid_d5s = 1.0\n", "not a feature of the notebook"},
        {"[ml.weights]\nbasis_arcus_mid_ema30s = 1.0\n", "not a feature of the notebook"},
        {"[pricing]\nmodel = \"ml\"\n[bybit]\nenabled = false\n[ml.weights]\nbybit_mid_d5s = 1.0\n", "[bybit] enabled"},
        {"[pricing]\nmodel = \"ml\"\n[futures]\nenabled = false\n[ml.weights]\nbasis_binance_futures_mid_ema30s = 1.0\n",
         "[futures] enabled"},
    });
}

TEST(MlConfig, ScalerSettings) {
    TempFile f;
    const std::string head = "[pricing]\nmodel = \"ml\"\n[ml.weights]\narcus_mid_r1s = 2e-5\nbinance_mid_r5s = -1e-5\n";
    f.write(head + "[ml]\nscaled = true\n[ml.mean]\narcus_mid_r1s = 0.0\nbinance_mid_r5s = 1e-7\n"
                   "[ml.scale]\narcus_mid_r1s = 2e-5\nbinance_mid_r5s = 5e-5\n");
    Config config = load_config(f.written);
    EXPECT_TRUE(config.ml.scaled);
    EXPECT_EQ(number_of(config.ml.scale, "binance_mid_r5s"), 5e-5);
    EXPECT_TRUE(Agent(config).ml->scaled);
    const std::string means = "[ml]\nscaled = true\n[ml.mean]\narcus_mid_r1s = 0.0\nbinance_mid_r5s = 0.0\n[ml.scale]\narcus_mid_r1s = 1.0\n";
    expect_refused(f, {
        {head + "[ml]\nscaled = true\n", "give the scaler"},  // the flag without the scaler: refused
        {head + "[ml.mean]\narcus_mid_r1s = 0.0\nbinance_mid_r5s = 0.0\n[ml.scale]\narcus_mid_r1s = 1.0\nbinance_mid_r5s = 1.0\n",
         "set ml.scaled = true"},
        {head + means, "missing: binance_mid_r5s"},
        {head + means + "binance_mid_r5s = 1.0\nbinance_futures_mid_r1s = 1.0\n", "not weighted: binance_futures_mid_r1s"},
        {head + means + "binance_mid_r5s = -1.0\n", "above 0"},
    });
}

TEST(MlConfig, LightgbmSettings) {
    TempFile f;
    const std::string head = "[pricing]\nmodel = \"ml\"\n[ml]\nkind = \"lightgbm\"\nlightgbm_model = \"" + LGBM_SMALL + "\"\n";
    std::string names = "features = [";
    for (size_t i = 0; i < LGBM_NAMES.size(); ++i) names += (i ? ", \"" : "\"") + LGBM_NAMES[i] + "\"";
    names += "]\n";
    Config config = load_config(f.write(head + names));
    EXPECT_EQ(config.ml.kind, "lightgbm");
    EXPECT_EQ(strings(config.ml.features), LGBM_NAMES);
    Agent agent(config);
    EXPECT_NE(dynamic_cast<ml::LightGBMPricer*>(agent.ml.get()), nullptr);
    EXPECT_EQ(agent.ml->venues, (std::vector<std::string> {"arcus", "binance", "binance_futures"}));
    std::string scaled_names = names;
    scaled_names.replace(0, std::string("features").size(), "scaled = true\nfeatures");
    expect_refused(f, {
        {"[ml]\nkind = \"xgboost\"\n", "ml.kind must be"},
        {"[ml]\nkind = \"lightgbm\"\n[ml.weights]\narcus_mid_r1s = 1.0\n", "is the linear model"},
        {"[ml]\nfeatures = [\"arcus_mid_r1s\"]\n", "are for ml.kind = \"lightgbm\""},
        {"[pricing]\nmodel = \"ml\"\n[ml]\nkind = \"lightgbm\"\n", "needs ml.lightgbm_model"},
        {"[pricing]\nmodel = \"ml\"\n[ml]\nkind = \"lightgbm\"\nlightgbm_model = \"" + (f.dir / "none.txt").string() + "\"\n" + names,
         "No such file"},
        {head + "features = [\"arcus_mid_r1s\", \"binance_mid_d5s\", \"binance_futures_mid_r5s\"]\n", "reads 4 features"},
        {head + "features = [\"arcus_mid_r1s\", \"okx_mid_r1s\", \"binance_futures_mid_r5s\", \"binance_mid_r1s\"]\n",
         "not a feature of the notebook"},
        {head + "features = [\"arcus_mid_r1s\", \"bybit_mid_r5s\", \"binance_futures_mid_r5s\", \"binance_mid_r1s\"]\n[bybit]\nenabled = false\n",
         "[bybit] enabled"},
        {head + scaled_names + "[ml.mean]\narcus_mid_r1s = 0.0\n[ml.scale]\narcus_mid_r1s = 1.0\n",
         "must give every feature of ml.features"},
        {head + names + "[futures]\nenabled = false\n", "[futures] enabled"},
    });
    // with the basis pricing the model file is not read: a path to switch to later may not exist yet
    f.write("[ml]\nkind = \"lightgbm\"\nlightgbm_model = \"" + (f.dir / "later.txt").string() + "\"\n");
    EXPECT_EQ(load_config(f.written).pricing.model, "basis");
}

TEST(MlConfig, ATargetOffFuturesNeedsTheFuturesFeed) {
    TempFile f;
    f.write("[pricing]\nmodel = \"ml\"\n[futures]\nenabled = false\n[ml]\ntarget_base = \"basis\"\n"
            "target_window_s = 30\n[ml.weights]\narcus_mid_r1s = 0.5\n");
    EXPECT_LOAD_ERROR(f.written, {}, "target_base = \"basis\" prices off Binance futures");
}

TEST(OpenGuardConfig, IsCheckedAndTheGldAndNvdaConfigsTurnItOn) {
    EXPECT_TRUE(Config().open_guard.opens.empty());  // off by default
    for (const std::string bad : {"[\"Sunday 18:00\"]", "[18]"})
        EXPECT_NE(load_error(std::nullopt, {"open_guard.opens=" + bad}), "") << bad;
    EXPECT_LOAD_ERROR(std::nullopt, (std::vector<std::string> {"open_guard.opens=[\"Sun 18:00\"]", "open_guard.tz=\"Mars/Olympus\""}),
                      "no time zone");
    EXPECT_LOAD_ERROR(std::nullopt, (std::vector<std::string> {"open_guard.limit_share=1.0"}), "open_guard needs");
    auto gld = load_config(repo("config.gld-both.toml")).open_guard;
    EXPECT_EQ(strings(gld.opens), (std::vector<std::string> {"Sun 18:00", "Mon-Thu 18:00", "Mon-Fri 09:30"}));
    EXPECT_EQ(gld.tz, "America/New_York");
    EXPECT_EQ(gld.before_s, 60.0);
    EXPECT_EQ(gld.after_s, 900.0);
    EXPECT_EQ(gld.limit_share, 0.0);
    auto nvda = load_config(repo("config.nvda-both.toml")).open_guard;
    EXPECT_EQ(strings(nvda.opens), (std::vector<std::string> {"Sun 18:00", "Mon-Fri 04:00", "Mon-Fri 09:30"}));
    for (const std::string name : {"gld", "nvda"}) {  // the long ones as their two-sided pairs
        auto one = load_config(repo("config." + name + ".toml")).open_guard;
        auto both = load_config(repo("config." + name + "-both.toml")).open_guard;
        EXPECT_EQ(reprs(one.opens), reprs(both.opens)) << name;
        EXPECT_EQ(std::tie(one.tz, one.before_s, one.after_s, one.limit_share),
                  std::tie(both.tz, both.before_s, both.after_s, both.limit_share))
            << name;
    }
    for (const std::string name : {"btc-both", "eth-both", "sol-both", "eth-ml-both"})  // crypto has no open
        EXPECT_TRUE(load_config(repo("config." + name + ".toml")).open_guard.opens.empty()) << name;
}

TEST(OpenHaltConfig, IsCheckedAndTheStockConfigsTurnItOnForThe0930Open) {
    EXPECT_TRUE(Config().open_halt.opens.empty());  // off by default
    EXPECT_LOAD_ERROR(std::nullopt, (std::vector<std::string> {"open_halt.opens=[\"Mon-Fri 09:30\"]", "open_halt.tz=\"Mars/Olympus\""}),
                      "no time zone");
    EXPECT_LOAD_ERROR(std::nullopt, (std::vector<std::string> {"open_halt.after_s=7200"}), "open_halt needs");
    // every stock: 10 min before to 10 min after (20 Oct, the owner's; was 60 s / 2 min, then 15 / 15 min)
    for (const std::string name : {"nvda", "nvda-both", "gld", "gld-both", "spy-both", "qqq-both"}) {
        auto h = load_config(repo("config." + name + ".toml")).open_halt;
        EXPECT_EQ(strings(h.opens), (std::vector<std::string> {"Mon-Fri 09:30"})) << name;
        EXPECT_EQ(std::tie(h.tz, h.before_s, h.after_s), std::make_tuple(std::string("America/New_York"), 600.0, 600.0)) << name;
    }
    for (const std::string name : {"btc-both", "eth-both", "sol-both", "eth-ml-both", "btc-ml-both"})  // crypto has no open
        EXPECT_TRUE(load_config(repo("config." + name + ".toml")).open_halt.opens.empty()) << name;
}

TEST(SessionConfig, TheMarketSpecReadsTheHours) {
    auto spec = MarketSpec::from_json(parse_json(R"({
        "marketId": 28, "marketDisplayName": "NVDA-USD", "tickSize": "0.01", "stepSize": "0.01",
        "minOrderSize": "0.01", "minOrderNotional": "5", "maxOrderSize": "10000", "isOutsideRth": true,
        "regularTradingHours": {"startSecondsOfDay": 14400, "endSecondsOfDay": 72000,
                                "timezone": "America/New_York", "isOvernight": false}})"));
    EXPECT_EQ(spec.hours, (Hours {14400, 72000, "America/New_York"}));
    EXPECT_EQ(spec.outside_rth, true);
    EXPECT_EQ(btc_spec().hours, std::nullopt);
    EXPECT_EQ(btc_spec().outside_rth, std::nullopt);
}

// C++ only: --set with an inline table keeps its keys in the order written, as Python's dict does
// (the settings lines, and the order of an ML model's features).
TEST(ConfigOverrides, AnInlineTableKeepsItsKeysInTheOrderWritten) {
    Config c = load_config(std::nullopt, {"ml.weights={intercept = 0.1, binance_futures_mid_r1s = 1.0, arcus_imb1 = 2.0}"});
    std::vector<std::string> keys;
    for (const auto& [k, v] : numbers(c.ml.weights)) keys.push_back(k);
    EXPECT_EQ(keys, (std::vector<std::string> {"intercept", "binance_futures_mid_r1s", "arcus_imb1"}));
    bool found = false;
    for (const auto& line : describe(c))
        if (line.starts_with("[ml] weights={'intercept': 0.1, 'binance_futures_mid_r1s': 1.0, 'arcus_imb1': 2.0} ")) found = true;
    EXPECT_TRUE(found);
}
