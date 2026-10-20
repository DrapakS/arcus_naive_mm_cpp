// Ported from tests/test_portfolio.py: inventory by asset class across the bots, each bot its own
// process on its own market, the positions shared through files, the class's lean moving every
// bot's markups.
#include <gtest/gtest.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "agent.hpp"
#include "config.hpp"
#include "feed_helpers.hpp"
#include "portfolio.hpp"
#include "strategy_helpers.hpp"
#include "test_util.hpp"
#include "trading/credentials.hpp"
#include "util/json.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/nmm-portfolio-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { fs::remove_all(path); }
};

// One bot's side of the files: its position and mark as the test sets them, a shared clock.
struct Bot {
    double position = 0.0, mark = 100.0;
    std::unique_ptr<Portfolio> p;

    Bot(const fs::path& dir, const std::string& market, const FakeClock& clock, double weight = 1.0,
        const std::string& asset_class = "crypto", std::optional<int64_t> pid = {}, double shift_bp = 0.0) {
        PortfolioOptions o;
        o.weight = weight;
        o.limit_usd = 1500.0;
        o.factor = 0.3;
        o.shift_bp = shift_bp;
        o.position = [this] { return position; };
        o.mark = [this] { return std::optional(mark); };
        o.clock = clock.fn();
        o.pid = pid ? *pid : static_cast<int64_t>(std::hash<std::string> {}(market) % 100'000);
        p = std::make_unique<Portfolio>(dir.string(), market, asset_class, o);
    }
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;
};

std::set<std::string> markets(const Portfolio& p) {
    std::set<std::string> out;
    for (const auto& h : p.others) out.insert(h.market);
    return out;
}

bool any_contains(const std::vector<std::string>& messages, const std::string& part) {
    return std::any_of(messages.begin(), messages.end(), [&](const std::string& m) { return m.find(part) != std::string::npos; });
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

Decimal round6(double x) { return Decimal(std::format("{:.6f}", x)); }

std::string load_error(const std::vector<std::string>& overrides) {
    try {
        load_config(std::nullopt, overrides);
    } catch (const std::invalid_argument& err) {
        return err.what();
    }
    return "";
}

}  // namespace

TEST(Portfolio, TheClassExposureIsTheWeightedPositionsOfItsBotsOursAsItIsNow) {
    TempDir tmp;
    FakeClock clock(1'000.0);
    Bot btc(tmp.path, "BTC-USD", clock), eth(tmp.path, "ETH-USD", clock, 1.3);
    btc.position = 0.009, btc.mark = 82'000.0;  // $738 long
    eth.position = 0.2, eth.mark = 2'400.0;  // $480 long, x1.3 = $624
    btc.p->publish(), eth.p->publish();
    btc.p->read(), eth.p->read();
    EXPECT_EQ(markets(*btc.p), (std::set<std::string> {"ETH-USD"}));
    EXPECT_NEAR(btc.p->others_usd, 624.0, 1e-9);
    EXPECT_NEAR(btc.p->exposure(0.009, 82'000.0), 738.0 + 624.0, 1e-9);
    double u = (738.0 + 624.0) / 1500.0;
    EXPECT_NEAR(btc.p->lean(0.009, 82'000.0), u, 1e-12);
    // long class: buys further out, sells nearer, in every market of it
    EXPECT_EQ(btc.p->multiplier("BUY", D("0.009"), Decimal(82'000)), round6(1 + 0.3 * u));
    EXPECT_EQ(btc.p->multiplier("SELL", D("0.009"), Decimal(82'000)), round6(1 - 0.3 * u));
    EXPECT_LT(eth.p->multiplier("SELL", D("0.2"), Decimal(2'400)), Decimal(1));
    EXPECT_LT(Decimal(1), eth.p->multiplier("BUY", D("0.2"), Decimal(2'400)));
    EXPECT_EQ(btc.p->lean(0.05, 82'000.0), 1.0);  // within -1..1
    std::string status = btc.p->status(D("0.009"), Decimal(82'000));
    EXPECT_NE(status.find("ETH-USD +624 $ (x1.3, 0 s ago); ours +738 $ -> +1,362 $, u +0.91: buys x1.27, sells x0.73"),
              std::string::npos)
        << status;
}

TEST(Portfolio, TheFileIsRewrittenOnAPositionChangeAndEachSecondAndReadAgainOnlyWhenItChanged) {
    TempDir tmp;
    FakeClock clock(1'000.0);
    Bot btc(tmp.path, "BTC-USD", clock), eth(tmp.path, "ETH-USD", clock);
    EXPECT_TRUE(btc.p->publish());
    EXPECT_FALSE(btc.p->publish());  // the same position within a second: not again
    btc.position = 0.001;
    EXPECT_TRUE(btc.p->publish());
    clock.advance(HEARTBEAT_S);
    EXPECT_TRUE(btc.p->publish());  // a second on: the heartbeat
    Json body = parse_json(read_file(btc.p->path));
    EXPECT_EQ(to_str(*jget(body, "market")), "BTC-USD");
    EXPECT_EQ(to_float(*jget(body, "position")), 0.001);
    EXPECT_EQ(to_float(*jget(body, "at")), 1'001.0);
    std::string name = fs::path(btc.p->path).filename().string();
    eth.p->read();
    auto first = eth.p->files.at(name).second;
    ASSERT_TRUE(first);
    eth.p->read();
    EXPECT_TRUE(eth.p->files.at(name).second == first);  // not changed: not read again
    for (const auto& entry : fs::directory_iterator(tmp.path))  // written aside, renamed over
        EXPECT_NE(entry.path().extension(), ".tmp") << entry.path();
}

TEST(Portfolio, TheFileIsTheOneThePythonBotsWrite) {
    // A Python bot and a C++ bot share one directory: json.dumps of the same dict, key for key.
    TempDir tmp;
    FakeClock clock(1'760'880'000.25);
    Bot btc(tmp.path, "BTC-USD", clock, 1.0, "crypto", 4242);
    btc.position = -0.0015, btc.mark = 82'000.5;
    btc.p->publish();
    EXPECT_EQ(btc.p->path, (tmp.path / "BTC-USD.4242.json").string());
    EXPECT_EQ(read_file(btc.p->path), R"({"market": "BTC-USD", "class": "crypto", "position": -0.0015, "mark": 82000.5, )"
                                      R"("weight": 1.0, "at": 1760880000.25, "pid": 4242})");
    // ... and reads theirs, integers where floats could be included
    std::ofstream(tmp.path / "ETH-USD.77.json")
        << R"({"market": "ETH-USD", "class": "crypto", "position": 1, "mark": 2400, "weight": 1.3, "at": 1760880000, "pid": 77})";
    std::ofstream(tmp.path / "SOL-USD.78.json") << R"({"market": "SOL-USD", "class": "crypto"})";  // not a bot's: left out
    btc.p->read();
    EXPECT_EQ(markets(*btc.p), (std::set<std::string> {"ETH-USD"}));
    EXPECT_NEAR(btc.p->others_usd, 1.3 * 2400, 1e-9);
    EXPECT_EQ(btc.p->other("ETH-USD")->pid, 77);
    EXPECT_FALSE(btc.p->files.at("SOL-USD.78.json").second);
}

TEST(Portfolio, AStoppedBotAnotherClassAndAnotherProcessOnOurMarketAreLeftOut) {
    LogCapture log("naive_mm.portfolio", logging::WARNING);
    TempDir tmp;
    FakeClock clock(1'000.0);
    Bot btc(tmp.path, "BTC-USD", clock);
    Bot eth(tmp.path, "ETH-USD", clock);
    Bot twin(tmp.path, "BTC-USD", clock, 1.0, "crypto", 4242);  // a second process on BTC
    Bot gold(tmp.path, "GLD-USD", clock, 1.0, "metals");
    for (Bot* b : {&eth, &twin, &gold}) {
        b->position = 1.0;
        b->p->publish();
    }
    btc.p->read();
    EXPECT_EQ(markets(*btc.p), (std::set<std::string> {"ETH-USD"}));
    EXPECT_TRUE(any_contains(log.messages(), "another bot on BTC-USD (pid 4242), not counted")) << join(log.messages(), "\n");
    clock.advance(31);  // ETH's bot stopped writing
    btc.p->read();
    EXPECT_TRUE(btc.p->others.empty());
    EXPECT_EQ(btc.p->others_usd, 0.0);
    EXPECT_TRUE(any_contains(log.messages(), "not rewritten for 31 s (its bot stopped?), left out")) << join(log.messages(), "\n");
    eth.p->close();
    EXPECT_FALSE(fs::exists(eth.p->path));
}

TEST(Portfolio, TwoOtherBotsOnOneMarketTheNewerCounts) {
    TempDir tmp;
    FakeClock clock(1'000.0);
    Bot btc(tmp.path, "BTC-USD", clock);
    Bot old(tmp.path, "ETH-USD", clock, 1.0, "crypto", 1), fresh(tmp.path, "ETH-USD", clock, 1.0, "crypto", 2);
    old.position = 1.0;
    old.p->publish();
    clock.advance(0.5);
    fresh.position = -1.0;
    fresh.p->publish();
    btc.p->read();
    ASSERT_NE(btc.p->other("ETH-USD"), nullptr);
    EXPECT_EQ(btc.p->other("ETH-USD")->position, -1.0);
}

TEST(Portfolio, TheStrategyQuotesTheBuyFurtherAndTheSellNearerWithTheClassLong) {
    LogCapture log("naive_mm.orders");
    TempDir tmp;
    FakeClock clock(1'000.0);
    Bot eth(tmp.path, "ETH-USD", clock, 1.3);
    eth.position = 0.5, eth.mark = 2'400.0;  // $1200 x1.3 = $1560: the class long past the limit, u = 1
    eth.p->publish();
    Bot btc(tmp.path, "BTC-USD", clock);
    btc.p->read();
    auto [gw0, flat] = both();
    auto [gw1, leaned] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.portfolio = btc.p.get(); });
    flat->evaluate(view(100'000.0));
    leaned->evaluate(view(100'000.0));
    std::map<std::string, Decimal> plain, moved;
    for (const auto& a : actions(*gw0)) plain[a.side] = a.price;
    for (const auto& a : actions(*gw1)) moved[a.side] = a.price;
    EXPECT_LT(moved["BUY"], plain["BUY"]);  // buy deeper, sell nearer
    EXPECT_LT(moved["SELL"], plain["SELL"]);
    // markup 0.001 x1.3 / x0.7 at u = 1
    EXPECT_EQ(moved["BUY"], D("99870.0"));
    EXPECT_EQ(moved["SELL"], D("100070.2"));
    auto messages = log.messages();
    EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& m) {
        return m.find(">>> PLACE BUY") != std::string::npos && m.find("class crypto +1,560 $ (u +1.00): markup x1.30") != std::string::npos;
    })) << join(messages, "\n");
}

TEST(Portfolio, TheShiftMovesBothQuotesAgainstTheClassLean) {
    LogCapture log("naive_mm.orders");
    TempDir tmp;
    FakeClock clock(1'000.0);
    Bot eth(tmp.path, "ETH-USD", clock, 1.3);
    eth.position = 0.5, eth.mark = 2'400.0;  // the class long past the limit: u = 1
    eth.p->publish();
    Bot btc(tmp.path, "BTC-USD", clock, 1.0, "crypto", {}, 2.0);
    btc.p->read();
    EXPECT_EQ(btc.p->shift(Decimal(0), Decimal(100'000)), D("0.0002"));
    eth.position = -0.5;  // short: both up
    eth.p->publish();
    btc.p->read();
    EXPECT_EQ(btc.p->shift(Decimal(0), Decimal(100'000)), D("-0.0002"));
    EXPECT_NE(btc.p->status(Decimal(0), Decimal(100'000)).find("u -1.00: buys x0.70, sells x1.30, quotes +2.00 bp"), std::string::npos)
        << btc.p->status(Decimal(0), Decimal(100'000));
    eth.position = 0.5;
    eth.p->publish();
    btc.p->read();
    auto [gw, leaned] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.portfolio = btc.p.get(); });
    leaned->evaluate(view(100'000.0));
    std::map<std::string, Decimal> moved;
    for (const auto& a : actions(*gw)) moved[a.side] = a.price;
    // from 100000 x (1 - 2 bp): the buy 99980 x (1 - 0.0013), the sell (99980 + 0.1) x (1 + 0.0007)
    EXPECT_EQ(moved["BUY"], D("99850.0"));
    EXPECT_EQ(moved["SELL"], D("100050.1"));
    auto messages = log.messages();
    EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& m) {
        return m.find(">>> PLACE BUY") != std::string::npos && m.find("markup x1.30, quotes -2.00 bp") != std::string::npos;
    })) << join(messages, "\n");
    Portfolio off(tmp.path.string(), "X", "crypto");
    EXPECT_EQ(off.shift(Decimal(1), Decimal(1)), Decimal(0));  // off by default
}

TEST(Portfolio, OnOnlyLiveAndChecked) {
    EXPECT_FALSE(Config().portfolio.enabled);  // off by default
    EXPECT_NE(load_error({"portfolio.enabled=true"}).find("asset_class"), std::string::npos);
    EXPECT_NE(load_error({"portfolio.enabled=true", "portfolio.asset_class=\"crypto\"", "portfolio.factor=1.0"}).find("factor"),
              std::string::npos);
    EXPECT_NE(load_error({"portfolio.enabled=true", "portfolio.asset_class=\"crypto\"", "portfolio.shift_bp=6"}).find("shift_bp"),
              std::string::npos);
    EXPECT_EQ(load_error({"portfolio.enabled=true", "portfolio.asset_class=\"crypto\"", "portfolio.interval_ms=20"}),
              "portfolio needs interval_ms within 50..10000 and stale_s of 3 s (3 heartbeats) at least");
    TempDir tmp;
    std::vector<std::string> sets {"portfolio.enabled=true", "portfolio.asset_class=\"crypto\"",
                                   std::format("portfolio.dir=\"{}\"", tmp.path.string())};
    auto with = [&sets](const std::string& extra) {
        auto all = sets;
        all.push_back(extra);
        return load_config(std::nullopt, all);
    };
    Agent paper(with("trading.mode=\"paper\""));
    EXPECT_FALSE(paper.portfolio);  // paper: never
    auto creds = ArcusCredentials::make("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", 1,
                                        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    Agent live(with("trading.mode=\"live\""), false, creds);
    ASSERT_TRUE(live.portfolio);
    EXPECT_EQ(live.portfolio->dir, (tmp.path / "live").string());
    EXPECT_EQ(fs::path(live.portfolio->path).filename().string(), std::format("BTC-USD.{}.json", ::getpid()));
}

TEST(Portfolio, TheTwoSidedCryptoConfigsHaveItOn) {
    auto load = [](const std::string& name) { return load_config(std::string(NMM_SOURCE_DIR) + "/config." + name + ".toml"); };
    if (!load("btc-both").portfolio.enabled)
        GTEST_SKIP() << "the configs are not yet those of the Python bot's cc695b9 (no [portfolio] in config.btc-both.toml)";
    for (auto [name, weight] : std::vector<std::pair<std::string, double>> {
             {"btc-both", 1.0}, {"btc-ml-both", 1.0}, {"eth-both", 1.3}, {"eth-ml-both", 1.3}, {"sol-both", 1.7}}) {
        const PortfolioConfig pf = load(name).portfolio;
        EXPECT_TRUE(pf.enabled) << name;
        EXPECT_EQ(pf.asset_class, "crypto") << name;
        EXPECT_EQ(pf.weight, weight) << name;
        // the live ML bots: with their $2500 limit (19 Oct evening; C++ checkout: btc-ml-both is the live set)
        EXPECT_EQ(pf.limit_usd, name == "eth-ml-both" || name == "btc-ml-both" ? 2500.0 : 1500.0) << name;
        EXPECT_EQ(pf.factor, 0.3) << name;  // 0.3 again from 20 Oct evening
        EXPECT_EQ(pf.shift_bp, 0.3) << name;
        EXPECT_EQ(pf.interval_ms, 100.0) << name;
    }
    EXPECT_FALSE(load("gld-both").portfolio.enabled);  // one bot a class: its own skew is the class's
    // C++ checkout: the US stock bots a class of their own (20 Oct, the owner's), weights their volatility over BTC's
    for (auto [name, weight] : std::vector<std::pair<std::string, double>> {{"nvda-both", 1.1}, {"spy-both", 0.35}, {"qqq-both", 0.6}}) {
        const PortfolioConfig pf = load(name).portfolio;
        EXPECT_TRUE(pf.enabled) << name;
        EXPECT_EQ(pf.asset_class, "stocks") << name;
        EXPECT_EQ(pf.weight, weight) << name;
        EXPECT_EQ(pf.limit_usd, 1500.0) << name;
        EXPECT_EQ(pf.factor, 0.3) << name;  // as crypto, from 20 Oct evening
        EXPECT_EQ(pf.shift_bp, 0.3) << name;
    }
}
