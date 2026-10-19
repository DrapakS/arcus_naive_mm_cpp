// The agent's guards on every frame, ported from the agent-level tests of tests/test_lag.py,
// test_take_guard.py, test_drift.py, test_futures_feed.py, test_trades_feed.py, test_bybit_feed.py
// and test_tob_lines.py.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <gtest/gtest.h>

#include <regex>

#include "agent_helpers.hpp"
#include "feed_helpers.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

void place_the_order(Bot& b, bool long_position) {
    Agent& agent = *b.agent;
    agent.reference_book.apply_snapshot(lv("100000"), lv("100000.1"), 1);
    agent.venue_book.apply_snapshot(lv("99990"), lv("99990.1"), 1);
    frame(agent, agent.venue_book);
    frame(agent, agent.reference_book);
    if (long_position) b.gw->add_pos(Decimal("0.001"));
    evaluate(agent);
    b.gw->ack();
}

// test_lag._lag_agent: only the lag guard (200 ms, pause 50 ms) and a working order: the buy when
// flat, the closing sell when `long_position`. The guard reads the bot's clock.
Bot lag_agent(bool long_position = false) {
    Config c;
    c.pricing.window = c.pricing.min_samples = 1;
    c.guard.drawdown = c.futures.drawdown = c.bybit.drawdown = c.arcus_guard.drawdown = 0;
    c.trades.drawdown = c.trend.drawdown = 0;
    c.lag.max_ms = 200.0;
    c.lag.pause_ms = 50.0;
    Bot b;
    b.agent = std::make_unique<Agent>(c);
    EXPECT_EQ(b->guards, (std::vector<Pauses*> {b->lag_guard.get()}));
    EXPECT_TRUE(b->loop_watch->on_lag);
    b->lag_guard->clock = b.clock.fn();
    b.gw = install(*b.agent, opts(0.001, 0.01, b->guards));
    place_the_order(b, long_position);
    return b;
}

// A Binance futures ticker the bot reads `read_after_ms` after the exchange stamped it.
void ticker(Agent& agent, double read_after_ms) {
    OrderBook& book = *agent.futures_book;
    book.apply_snapshot(lv("100000.5"), lv("100000.6"), book.seq + 1, 1'760'000'000'000'000);
    book.local_ts_us = *book.exchange_ts_us + static_cast<int64_t>(read_after_ms * 1000);
    agent.on_book_update(book);
}

void bybit_late(Agent& agent, double read_after_ms) {
    OrderBook& book = *agent.bybit_book;
    book.apply_snapshot(lv("100000.5"), lv("100000.6"), book.seq + 1, 1'760'000'000'000'000);
    book.local_ts_us = *book.exchange_ts_us + static_cast<int64_t>(read_after_ms * 1000);
    agent.on_book_update(book);
}

// test_futures_feed._futures / test_bybit_feed._bybit: the top is now `bid` / `bid` + 0.1.
void futures(Agent& agent, const std::string& bid) { top(agent, *agent.futures_book, bid, agent.futures_book->seq + 1); }
void bybit(Agent& agent, const std::string& bid) { top(agent, *agent.bybit_book, bid, agent.bybit_book->seq + 1); }

// test_futures_feed._trend_agent: only the trend guard (120 s, 30 bp, pause 50 ms).
Bot trend_agent(bool long_position = false, const std::string& direction = "long") {
    Config c;
    c.pricing.window = c.pricing.min_samples = 1;
    c.guard.drawdown = c.futures.drawdown = c.bybit.drawdown = c.arcus_guard.drawdown = 0;
    c.trades.drawdown = c.lag.max_ms = 0;
    c.trend.window_s = 120.0;
    c.trend.drawdown = 0.003;
    c.trend.pause_s = 0.05;
    c.trading.direction = direction;
    Bot b;
    b.agent = std::make_unique<Agent>(c);
    EXPECT_EQ(b->guards, (std::vector<Pauses*> {b->trend_guard.get()}));
    EXPECT_EQ(b->trend_book, b->futures_book.get());
    b->trend_guard->clock = b.clock.fn();
    b.gw = install(*b.agent, opts(0.001, 0.01, b->guards, direction));
    Agent& agent = *b.agent;
    agent.reference_book.apply_snapshot(lv("100000"), lv("100000.1"), 1);
    agent.venue_book.apply_snapshot(lv("99990"), lv("99990.1"), 1);
    frame(agent, agent.venue_book);
    frame(agent, agent.reference_book);
    futures(agent, "100000");
    if (long_position) b.gw->add_pos(Decimal("0.001"));
    evaluate(agent);
    b.gw->ack();
    return b;
}

// test_trades_feed._sweep_agent: only the trades guard (300 ms, 0.3 bp, pause 50 ms).
Bot sweep_agent(bool long_position = false) {
    Config c;
    c.pricing.window = c.pricing.min_samples = 1;
    c.guard.drawdown = c.futures.drawdown = c.bybit.drawdown = c.arcus_guard.drawdown = 0;
    c.trend.drawdown = c.lag.max_ms = 0;
    c.trades.window_ms = 300.0;
    c.trades.drawdown = 0.00003;
    c.trades.pause_ms = 50.0;
    Bot b;
    b.agent = std::make_unique<Agent>(c);
    EXPECT_EQ(b->guards, (std::vector<Pauses*> {b->trades_guard.get()}));
    b->trades_guard->clock = b.clock.fn();
    b.gw = install(*b.agent, opts(0.001, 0.01, b->guards));
    place_the_order(b, long_position);
    return b;
}

constexpr int64_t T0 = 1'700'000'000'000'000;  // a trade's time, µs

}  // namespace

// ------------------------------------------------------------------ lag

TEST(AgentLag, AFuturesTickerReadLatePullsTheOrderAndHoldsOffPlacing) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = lag_agent();
    Agent& agent = *b.agent;
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}}));
    ticker(agent, 150);  // late, but under the limit
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    ticker(agent, 350);
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}, {"cancel", "BUY"}}));
    EXPECT_EQ(b.gw->why, "late data: Binance futures ticker read 350 ms after the exchange's time for it");
    EXPECT_TRUE(contains(log.messages(),
                         "!!! LATE Binance futures ticker read 350 ms after the exchange's time for it (limit 200 ms): "
                         "both orders pulled, nothing placed until the data is on time for 50 ms"))
        << join(log.messages(), "\n");
    EXPECT_TRUE(agent.strategy->paused("BUY") && agent.strategy->paused("SELL"));
    b.gw->confirm_cancel();
    b.clock.advance(0.03);
    ticker(agent, 2500);  // later still: the pause runs on, without another line
    int late_lines = 0;
    for (auto& m : log.messages()) late_lines += m.rfind("!!! LATE", 0) == 0;
    EXPECT_EQ(late_lines, 1);
    evaluate(agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));  // nothing placed meanwhile
    b.clock.advance(0.06);
    loop.run_for(0.12);  // the resume timer runs on the loop's own clock
    EXPECT_TRUE(A::changed(agent).is_set());
    // from the first late ticker to 50 ms after the second
    EXPECT_TRUE(contains(log.messages(), "--- late data pause over: 80 ms, extended 1 times, the latest data read up to 2500 ms late"))
        << join(log.messages(), "\n");
    evaluate(agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel", "place"}));
}

TEST(AgentLag, ALateSpotTradeOrALateLoopPullsTheClosingSellToo) {
    Loop loop;
    {
        Bot b = lag_agent(true);
        EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}}));
        b->on_trade(100'000.0, 1'760'000'000'000'000, 1'760'000'000'120'000);  // read 120 ms after: on time
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
        b->on_trade(100'000.0, 1'760'000'000'000'000, 1'760'000'000'480'000);  // 480 ms
        EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
        EXPECT_EQ(b.gw->why, "late data: Binance spot trade read 480 ms after the exchange's time for it");
    }
    Bot b = lag_agent();
    b->loop_watch->observe(150.0);  // a stall under the limit
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    b->loop_watch->observe(640.0);  // everything waited that long
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}, {"cancel", "BUY"}}));
    EXPECT_EQ(b.gw->why, "late data: event loop 640 ms late");
}

TEST(AgentLag, TheLagIsReadOnTheExchangeClock) {
    Loop loop;
    Bot b = lag_agent();
    b.gw->clock_offset_ms = 400.0;  // the host clock runs 400 ms ahead of Arcus's
    ticker(*b.agent, 450);  // 50 ms on the exchange's clock
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    b.gw->clock_offset_ms = -100.0;  // 100 ms behind
    ticker(*b.agent, 150);  // 250 ms
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));
    EXPECT_EQ(b.gw->why, "late data: Binance futures ticker read 250 ms after the exchange's time for it");
}

TEST(AgentLag, TheHealthLineAndTheStatusShowThePauses) {
    Loop loop;
    LogCapture log("naive_mm.agent");
    Bot b = lag_agent();
    Agent& agent = *b.agent;
    EXPECT_EQ(agent.health().find("late data"), std::string::npos);  // the first report starts the count
    b.clock.advance(1.0);
    agent.loop_watch->observe(300.0);
    b.clock.advance(0.04);
    agent.loop_watch->observe(300.0);  // extends the pause, to 90 ms in all
    b.clock.advance(7.96);
    std::string health = agent.health();
    EXPECT_TRUE(std::regex_search(health, std::regex(R"(; nothing placed for late data 1\.0% of the time$)"))) << health;  // 90 ms of 9 s
    EXPECT_EQ(agent.health().find("late data"), std::string::npos);  // nothing since: the line stays short
    agent.log_status();
    EXPECT_TRUE(contains(log.messages(), "lag guard: 1 pause for late data, 90 ms in all")) << join(log.messages(), "\n");
}

TEST(AgentLag, LagSettings) {
    Loop loop;
    Config config;
    EXPECT_EQ(config.lag.max_ms, 200.0);
    EXPECT_EQ(config.lag.pause_ms, 500.0);
    Agent agent(config);
    ASSERT_TRUE(agent.lag_guard);
    EXPECT_NE(std::find(agent.guards.begin(), agent.guards.end(), agent.lag_guard.get()), agent.guards.end());
    EXPECT_EQ(agent.lag_guard->max_ms, 200.0);
    EXPECT_EQ(agent.lag_guard->pause_s, 0.5);
    TempFile f;
    f.write("[lag]\nmax_ms = 0\n");
    Agent off(load_config(f.written));
    EXPECT_FALSE(off.lag_guard);  // off
    EXPECT_FALSE(off.loop_watch->on_lag);
    for (const char* bad : {"max_ms = 5", "max_ms = -1", "max_ms = 70000", "pause_ms = -1"}) {
        f.write(std::format("[lag]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "lag");
    }
}

TEST(AgentLag, ALateBybitTickerPausesOnlyPastItsOwnLimit) {
    Loop loop;
    {
        Bot b = lag_agent();
        bybit_late(*b.agent, 5000);  // by default a late Bybit ticker never pauses: a model feature, not the price
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    }
    Bot b = lag_agent();
    b->config.lag.bybit_max_ms = 1000.0;
    bybit_late(*b.agent, 600);  // past max_ms, under Bybit's own limit
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    bybit_late(*b.agent, 1200);
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}, {"cancel", "BUY"}}));
    EXPECT_EQ(b.gw->why, "late data: Bybit ticker read 1200 ms after the exchange's time for it");
}

TEST(AgentLag, BybitLagSettings) {
    EXPECT_EQ(Config().lag.bybit_max_ms, 0.0);
    TempFile f;
    f.write("[lag]\nbybit_max_ms = 1000\n");
    EXPECT_EQ(load_config(f.written).lag.bybit_max_ms, 1000.0);
    for (const char* bad : {"bybit_max_ms = 100", "bybit_max_ms = 70000"}) {  // under max_ms (200), over a minute
        f.write(std::format("[lag]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "bybit_max_ms");
    }
}

// ----------------------------------------------------------------- takes

TEST(AgentTakes, TheAgentPullsTheSideATakeHit) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Config config;
    config.takes.pause_ms = 1000;
    config.pricing.window = config.pricing.min_samples = 1;
    Agent agent(config);
    ASSERT_TRUE(agent.take_guard);
    EXPECT_NE(std::find(agent.guards.begin(), agent.guards.end(), agent.take_guard.get()), agent.guards.end());
    FakeGateway* gw = install(agent, opts(0.001, 0.01));
    gw->place("BUY", Decimal("99000"), Decimal("0.001"));
    gw->ack();
    A::on_arcus_take(agent, parse_json(R"([{"side": "SELL", "price": "2400", "size": "0.2", "takerAddress": "0x5c82bbe378"},
                                          {"side": "SELL", "price": "2399.9", "size": "0.1", "takerAddress": "0x5c82bbe378"}])")
                                .get_array());
    EXPECT_EQ(gw->sides(), (Sides {{"place", "BUY"}, {"cancel", "BUY"}}));
    EXPECT_EQ(gw->why, "take: Arcus taker sell $720 by 0x5c82bbe3");
    EXPECT_TRUE(contains(log.messages(), "!!! TAKE Arcus taker sell $720 by 0x5c82bbe3: no buying for 1000 ms")) << join(log.messages(), "\n");
    EXPECT_EQ(A::resume_timers(agent).size(), 1u);  // the resume timer
    EXPECT_TRUE(A::resume_timers(agent).count({agent.take_guard.get(), "BUY"}));
}

TEST(AgentTakes, TheAgentLogsAQuietTakerAndFeedsTheMid) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Config config;
    config.takes.pause_ms = 1000;
    config.takes.min_takes = 1;
    Agent agent(config);
    FakeClock clock(0.0);
    agent.take_guard->clock = clock.fn();
    OrderBook book("arcus", "BTC-USD");
    book.apply_snapshot(lv("99.9"), lv("100.1"), 1);  // mid 100
    A::score_takers(agent, book);
    clock.advance(0.1);
    A::on_arcus_take(agent, parse_json(R"([{"side": "SELL", "price": "100", "size": "8", "takerAddress": "0x38BDC44AD07A"}])").get_array());
    clock.advance(1.0);
    A::score_takers(agent, book);
    EXPECT_EQ(agent.take_guard->quiet, (std::set<std::string> {"0x38bdc44ad07a"}));
    EXPECT_TRUE(contains(log.messages(),
                         "--- taker 0x38bdc44a: the Arcus mid +0.00 bp its way 1000 ms after its takes (mean of the last 1): "
                         "under 0.2 bp, its takes pause nothing now"))
        << join(log.messages(), "\n");
}

TEST(AgentTakes, TheBtcAndEthConfigsTurnItOn) {
    for (std::string name : {"btc-both", "eth-both", "btc-ml-both", "eth-ml-both", "sol-both", "sol"}) {
        Config c = load_config(repo("config." + name + ".toml"));
        EXPECT_EQ(c.takes.min_usd, 0.0) << name;
        EXPECT_EQ(c.takes.pause_ms, 500.0) << name;
        EXPECT_EQ(c.takes.min_takes, 20) << name;
        EXPECT_EQ(c.takes.min_move, 0.00002) << name;
        EXPECT_EQ(c.takes.score_ms, 1000.0) << name;
        if (name.rfind("btc", 0) == 0 || name.rfind("eth", 0) == 0) {
            EXPECT_EQ(c.futures.window_ms, 100.0) << name;
            EXPECT_EQ(c.futures.pause_ms, 300.0) << name;
        }
    }
    EXPECT_EQ(load_config(repo("config.btc-both.toml")).futures.drawdown, 0.00002);
    EXPECT_EQ(load_config(repo("config.eth-both.toml")).futures.drawdown, 0.00002);
    EXPECT_EQ(Config().takes.pause_ms, 0);  // off by default
    EXPECT_EQ(Config().takes.min_takes, 0);
}

TEST(AgentTakes, TheTwoSidedConfigsHoldThePriceTheBookCameTo) {
    for (std::string name : {"btc-both", "eth-both", "btc-ml-both", "eth-ml-both", "sol-both", "nvda-both", "gld-both"}) {
        Config c = load_config(repo("config." + name + ".toml"));
        EXPECT_EQ(c.trading.never_lead, !name.starts_with("eth")) << name;
        EXPECT_EQ(c.trading.came_hold_ms, 300.0) << name;
    }
    for (std::string name : {"eth", "eth-both", "eth-ml-both"})  // ETH: never_lead off from 19 Oct evening
        EXPECT_FALSE(load_config(repo("config." + name + ".toml")).trading.never_lead) << name;
    EXPECT_EQ(Config().trading.came_hold_ms, 0);  // off by default
    for (std::string name : {"nvda-both", "gld-both"}) {  // a quiet perp top off hours is still the price
        Config c = load_config(repo("config." + name + ".toml"));
        EXPECT_EQ(c.futures.max_book_age_s, 10.0) << name;
        EXPECT_EQ(c.futures.stale_timeout_s, 30.0) << name;
    }
}

TEST(AgentTakes, SolQuotesTwiceItsOldMarkup) {
    for (std::string name : {"sol-both", "sol"})
        EXPECT_EQ(load_config(repo("config." + name + ".toml")).trading.markup, 0.00006) << name;  // 0.6 bp from 18 Oct
}

// ----------------------------------------------------------------- drift

TEST(AgentDrift, TheAgentFeedsTheWatchFromFuturesAndLogsEpisodes) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Config config;
    config.drift.markup_factor = 1.5;
    Agent agent(config);
    FakeClock clock(0.0);
    agent.drift->clock = clock.fn();
    agent.futures_book->apply_snapshot(lv("100000"), lv("100000.2"), 1);
    frame(agent, *agent.futures_book);
    clock.advance(10.0);
    agent.futures_book->apply_snapshot(lv("100003"), lv("100003.2"), 2);  // +0.3 bp: sells drifted
    frame(agent, *agent.futures_book);
    EXPECT_TRUE(agent.drift->against("SELL"));
    EXPECT_TRUE(contains(log.messages(), "!!! DRIFT futures mid +0.30 bp in 10 s: sell markup x1.5 while it lasts (and 2000 ms more)"))
        << join(log.messages(), "\n");
    EXPECT_EQ(A::drift_report(agent), "buys 0 times for 0 s, sells 1 times for 0 s; markup x1.5");
}

TEST(AgentDrift, TheDriftSectionAndSetOverrides) {
    TempFile f;
    f.write("[drift]\nmarkup_factor = 1.5\nthreshold = 0.00003\n");
    Config config = load_config(f.written);
    EXPECT_EQ(config.drift.markup_factor, 1.5);
    EXPECT_EQ(config.drift.threshold, 0.00003);
    EXPECT_EQ(config.drift.window_s, 10.0);
    Config off = load_config(f.written, {"drift.markup_factor=1", "trading.mode=paper"});
    EXPECT_EQ(off.drift.markup_factor, 1.0);
    EXPECT_EQ(off.trading.mode, "paper");
    for (const char* bad : {"markup_factor = 0.5", "threshold = 0.2", "window_s = 0"}) {
        f.write(std::format("[drift]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "drift");
    }
    EXPECT_LOAD_ERROR(std::nullopt, {"drift.markup_factor"}, "section.key=value");
    EXPECT_LOAD_ERROR(std::nullopt, {"drift.nope=1"}, "unknown config key");
    for (std::string name : {"btc-both", "eth-both", "btc-ml-both", "eth-ml-both"})  // on for BTC and ETH, x2.5 from 18 Oct
        EXPECT_EQ(load_config(repo("config." + name + ".toml")).drift.markup_factor, 2.5) << name;
}

// --------------------------------------------------------------- futures

TEST(AgentFutures, FuturesDropPullsABuyAndRisePullsASell) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    {
        Bot b = guarded_agent("BUY");  // both guards look at the last 10 ms
        futures(*b.agent, "100100");
        b.clock.advance(0.01);
        futures(*b.agent, "100100");
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));  // futures frames alone change nothing
        futures(*b.agent, "100080");  // 2 bp under the futures mean
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));
        EXPECT_TRUE(any_starts(log.messages(), "!!! DROP Binance futures bid 100080.00 is 2.00 bp under its mean 100100.00"))
            << join(log.messages(), "\n");
        EXPECT_TRUE(b->strategy->paused("BUY"));
        EXPECT_FALSE(b->strategy->paused("SELL"));
    }
    Bot b = guarded_agent("SELL");
    futures(*b.agent, "100100");
    b.clock.advance(0.01);
    futures(*b.agent, "100130");  // 3 bp over
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
}

TEST(AgentFutures, FuturesSettings) {
    Loop loop;
    Config config;
    EXPECT_TRUE(config.futures.enabled);
    EXPECT_EQ(config.futures.ws_url, "wss://fstream.binance.com");
    {
        Agent agent(config);
        // spot, futures, trades, Bybit, Arcus, trend, lag
        EXPECT_TRUE(agent.futures_guard);
        EXPECT_EQ(agent.guards.size(), 7u);
        EXPECT_EQ(agent.guards.back(), agent.lag_guard.get());
    }
    config.futures.drawdown = 0;
    EXPECT_FALSE(Agent(config).futures_guard);  // the feed stays, for the price lines
    config.futures.enabled = false;
    EXPECT_FALSE(Agent(config).futures_book);
    TempFile f;
    for (const char* bad : {"connections = 5", "drawdown = 1", "window_ms = 0"}) {
        f.write(std::format("[futures]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "futures");
    }
}

TEST(AgentFutures, ASharpFuturesFallPullsTheBuyAndHoldsOffBuying) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = trend_agent();
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}}));
    b.clock.advance(30);
    futures(*b.agent, "99800");  // 20 bp under the high of the last 2 minutes: buying on
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    b.clock.advance(30);
    futures(*b.agent, "99650");  // 35 bp under it
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}, {"cancel", "BUY"}}));
    EXPECT_TRUE(contains(log.messages(),
                         "!!! DROP trend: Binance futures mid 99650.05 is 35.00 bp under its high 100000.05 over the last 120 s: "
                         "no buying for 50 ms"))
        << join(log.messages(), "\n");
    b.gw->confirm_cancel();
    evaluate(*b.agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));  // no new buy during the pause
    b.clock.advance(0.06);
    loop.run_for(0.08);  // the resume timer runs on the loop's own clock
    EXPECT_TRUE(A::changed(*b.agent).is_set());
    EXPECT_TRUE(contains(log.messages(), "--- trend: Binance futures buying pause over: 50 ms, extended 0 times"));
    evaluate(*b.agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel", "place"}));
}

TEST(AgentFutures, AFuturesFallLeavesTheClosingSellAlone) {
    Loop loop;
    Bot b = trend_agent(true);
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}}));
    b.clock.advance(30);
    futures(*b.agent, "99500");  // 50 bp down: no buying, but the way out stays open
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    EXPECT_TRUE(b->strategy->paused("BUY"));
    EXPECT_FALSE(b->strategy->paused("SELL"));
}

TEST(AgentFutures, AShortBotHoldsOffSellingAfterASharpRally) {
    Loop loop;
    Bot b = trend_agent(false, "short");
    EXPECT_EQ(b->trend_guard->side, "SELL");
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}}));
    b.clock.advance(30);
    futures(*b.agent, "100400");  // 40 bp over the low of the last 2 minutes
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
    EXPECT_TRUE(b->strategy->paused("SELL"));
    EXPECT_FALSE(b->strategy->paused("BUY"));
}

TEST(AgentFutures, TrendSettings) {
    Loop loop;
    Config config;
    {
        Agent agent(config);
        EXPECT_EQ(agent.trend_guard->window_s, 120.0);
        EXPECT_EQ(agent.trend_guard->drawdown, 0.003);
        EXPECT_EQ(agent.trend_guard->pause_s, 60.0);
        EXPECT_EQ(agent.trend_guard->side, "BUY");
        EXPECT_EQ(agent.trend_book, agent.futures_book.get());
    }
    config.futures.enabled = false;
    Agent spot(config);
    EXPECT_EQ(spot.trend_book, &spot.reference_book);  // no futures: the spot mid
    TempFile f;
    f.write("[trend]\ndrawdown = 0\n");
    EXPECT_FALSE(Agent(load_config(f.written)).trend_guard);  // off
    f.write("[trend]\ndrawdown = 30\n");
    EXPECT_LOAD_ERROR(f.written, {}, "trend.drawdown = 30 is 300000 bp");
    for (const char* bad : {"window_s = 0", "pause_s = -1", "drawdown = -0.001", "window_s = 4000"}) {
        f.write(std::format("[trend]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "trend");
    }
}

// ---------------------------------------------------------------- trades

TEST(AgentTrades, ASweepOnBinanceSpotPullsTheBuyAndHoldsOffBuying) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = sweep_agent();
    Agent& agent = *b.agent;
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}}));
    agent.on_trade(100'000.0, T0, T0 + 2'000);
    b.clock.advance(0.1);
    agent.on_trade(99'998.5, T0, T0 + 2'000);  // 0.15 bp under the high: nothing
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    b.clock.advance(0.1);
    agent.on_trade(99'996.0, T0 + 200'000, T0 + 203'000);  // 0.4 bp under the high of the last 300 ms
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}, {"cancel", "BUY"}}));
    EXPECT_TRUE(contains(log.messages(),
                         "!!! DROP Binance trade 99996.00 is 0.40 bp under the high 100000.00 over the last 300 ms: "
                         "no buying for 50 ms (read 3 ms after the trade)"))
        << join(log.messages(), "\n");
    EXPECT_TRUE(agent.strategy->paused("BUY"));
    EXPECT_FALSE(agent.strategy->paused("SELL"));
    b.gw->confirm_cancel();
    evaluate(agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));  // no new buy during the pause
    b.clock.advance(0.06);
    loop.run_for(0.08);  // the resume timer runs on the loop's own clock
    EXPECT_TRUE(A::changed(agent).is_set());
    EXPECT_TRUE(contains(log.messages(), "--- Binance trades buying pause over: 50 ms, extended 0 times"));
    evaluate(agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel", "place"}));
}

TEST(AgentTrades, ASpikeUpThenBackTripsOnTheHighWhereTheMeanWouldNot) {
    Loop loop;
    Bot b = sweep_agent();
    b->on_trade(100'000.0, T0, T0);
    b.clock.advance(0.2);
    b->on_trade(100'005.0, T0, T0);  // a spike: the bid would follow it up
    b.clock.advance(0.01);
    b->on_trade(100'001.0, T0, T0);  // back near where it was, but 0.4 bp under the spike
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}, {"cancel", "BUY"}}));
}

TEST(AgentTrades, ARallyPullsTheClosingSellAndBuysAreLeftAlone) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = sweep_agent(true);
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}}));
    b->on_trade(100'000.0, T0, T0 + 1'000);
    b.clock.advance(0.05);
    b->on_trade(100'005.0, T0, T0 + 1'000);  // 0.5 bp over the low
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
    EXPECT_TRUE(any_starts(log.messages(), "!!! RISE Binance trade 100005.00 is 0.50 bp over the low 100000.00")) << join(log.messages(), "\n");
    EXPECT_TRUE(b->strategy->paused("SELL"));
    EXPECT_FALSE(b->strategy->paused("BUY"));
}

TEST(AgentTrades, TradesOlderThanTheWindowAndACutStreamAreForgotten) {
    Loop loop;
    Bot b = sweep_agent();
    SweepGuard& guard = *b->trades_guard;
    b->on_trade(100'000.0, T0, T0);
    b.clock.advance(0.31);  // the high is out of the window
    b->on_trade(99'996.0, T0, T0);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    EXPECT_EQ(guard.extremes(), (std::optional<std::pair<double, double>> {{99'996.0, 99'996.0}}));
    A::on_trades_reset(*b.agent);  // every connection down
    EXPECT_FALSE(guard.extremes());
    EXPECT_FALSE(A::last_trade(*b.agent));
    b->on_trade(99'990.0, T0, T0);  // the first trade after the reset has nothing to compare with
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
}

TEST(AgentTrades, TheFillContextShowsTheLastTradeAgainstTheWindow) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = sweep_agent();
    Agent& agent = *b.agent;
    b.gw->on_fill = [&agent](const OrderPtr& o, const Decimal& size) { A::on_fill(agent, o, size); };
    agent.on_trade(100'000.0, T0, T0);
    b.clock.advance(0.05);
    agent.on_trade(99'999.0, T0, T0);  // 0.1 bp under the high: no trip
    b.gw->fill_order("0.001");
    auto messages = log.messages();
    auto context = std::find_if(messages.begin(), messages.end(), [](const std::string& m) { return m.rfind("--- fill context", 0) == 0; });
    ASSERT_NE(context, messages.end());
    EXPECT_NE(context->find("; last Binance trade -0.10 bp vs the high of the last 300 ms, +0.00 vs the low; order stood "), std::string::npos)
        << *context;
}

TEST(AgentTrades, TradesSettings) {
    Loop loop;
    Config config;
    EXPECT_TRUE(config.trades.enabled);
    EXPECT_EQ(config.trades.ws_url, "");  // the [binance] ws_url
    {
        Agent agent(config);
        SweepGuard& guard = *agent.trades_guard;
        EXPECT_EQ(guard.window_s, 0.3);
        EXPECT_EQ(guard.drawdown, 0.00003);
        EXPECT_EQ(guard.pause_s, 0.2);
        EXPECT_NE(std::find(agent.guards.begin(), agent.guards.end(), &guard), agent.guards.end());
        auto feed = A::make_trades_feed(agent);
        EXPECT_EQ(feed->url, "wss://stream.binance.com:9443/stream?streams=btcusdt@aggTrade");
        EXPECT_EQ(feed->links.size(), 2u);
    }
    TempFile f;
    f.write("[binance]\nws_url = \"wss://data-stream.binance.vision\"\n");  // behind a geo-block
    {
        Agent agent(load_config(f.written));
        EXPECT_EQ(A::make_trades_feed(agent)->url, "wss://data-stream.binance.vision/stream?streams=btcusdt@aggTrade");
    }
    f.write("[trades]\ndrawdown = 0\n");
    EXPECT_FALSE(Agent(load_config(f.written)).trades_guard);  // the feed stays, for its delays in the health line
    f.write("[trades]\nenabled = false\n");
    EXPECT_FALSE(Agent(load_config(f.written)).trades_guard);
    f.write("[trades]\ndrawdown = 0.3\n");
    EXPECT_LOAD_ERROR(f.written, {}, "trades.drawdown = 0.3 is 3000 bp");
    for (const char* bad : {"connections = 5", "window_ms = 0", "pause_ms = -1", "stale_timeout_s = -1"}) {
        f.write(std::format("[trades]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "trades");
    }
}

TEST(AgentTrades, TheHealthLineNamesEachMeasure) {
    Loop loop;
    Agent agent {Config()};
    agent.loop_watch->observe(0.5);
    agent.loop_watch->observe(3.0);
    std::string health = agent.health();
    EXPECT_TRUE(std::regex_match(health, std::regex(R"(event loop lag p50 3\.0 / p99 3\.0 / max 3\.0 ms \(n 2\); cpu \d+% of a core)")))
        << health;
    health = agent.health();
    EXPECT_TRUE(std::regex_match(health, std::regex(R"(cpu \d+% of a core)"))) << health;  // each report starts over
}

TEST(AgentTrades, HealthSettings) {
    EXPECT_EQ(Config().agent.health_interval_s, 60.0);
    TempFile f;
    f.write("[agent]\nhealth_interval_s = 0\n");
    EXPECT_EQ(load_config(f.written).agent.health_interval_s, 0.0);  // off
    for (const char* bad : {"health_interval_s = 1", "health_interval_s = 4000"}) {
        f.write(std::format("[agent]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "health_interval_s");
    }
}

// ----------------------------------------------------------------- bybit

TEST(AgentBybit, BybitDropPullsABuyAndRisePullsASell) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    {
        Bot b = guarded_agent("BUY");
        bybit(*b.agent, "100100");
        b.clock.advance(0.01);  // a whole window behind the Bybit guard
        bybit(*b.agent, "100100");
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));  // Bybit frames alone change nothing
        bybit(*b.agent, "100080");  // 2 bp under the Bybit mean
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));
        EXPECT_TRUE(any_starts(log.messages(), "!!! DROP Bybit linear bid 100080.00 is 2.00 bp under its mean 100100.00 over the last"))
            << join(log.messages(), "\n");
        EXPECT_TRUE(b->strategy->paused("BUY"));
        EXPECT_FALSE(b->strategy->paused("SELL"));
    }
    Bot b = guarded_agent("SELL");
    bybit(*b.agent, "100100");
    b.clock.advance(0.01);
    bybit(*b.agent, "100130");  // 3 bp over
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
}

TEST(AgentBybit, BybitSettings) {
    Loop loop;
    Config config;
    EXPECT_TRUE(config.bybit.enabled);
    EXPECT_EQ(config.bybit.category, "linear");
    {
        Agent agent(config);
        EXPECT_TRUE(agent.bybit_guard);
        EXPECT_EQ(agent.bybit_book->venue, "bybit-linear");
    }
    config.bybit.drawdown = 0;
    EXPECT_FALSE(Agent(config).bybit_guard);  // the feed stays, for the price lines
    config.bybit.enabled = false;
    EXPECT_FALSE(Agent(config).bybit_book);
    EXPECT_FALSE(Agent(config).bybit_guard);
    TempFile f;
    f.write("[bybit]\ncategory = \"spot\"\nwindow_ms = 500\n");
    {
        Agent agent(load_config(f.written));
        EXPECT_EQ(agent.bybit_book->venue, "bybit-spot");
        EXPECT_EQ(agent.bybit_guard->window_s, 0.5);
    }
    for (const char* bad : {"category = \"inverse\"", "connections = 5", "drawdown = 1", "window_ms = 0"}) {
        f.write(std::format("[bybit]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "bybit");
    }
}

// ------------------------------------------------------------- tob lines

TEST(AgentTobLines, ALinePerBestBidChangeOfEachBook) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Config config;
    config.bybit.enabled = true;
    config.agent.tob_line_ms = 0;  // every change
    Agent agent(config);
    int64_t stamp = time_ns() / 1000 - 5'000;  // 5 ms ago: within the [lag] limit
    agent.futures_book->apply_snapshot(lv("100000"), lv("100000.2"), 1, stamp);
    frame(agent, *agent.futures_book);
    agent.futures_book->apply_snapshot(lv("100000", "2"), lv("100000.2"), 2, stamp);  // only a size: no line
    frame(agent, *agent.futures_book);
    agent.futures_book->apply_snapshot(lv("100000.1", "2"), lv("100000.2"), 3, stamp + 1500);
    frame(agent, *agent.futures_book);
    for (OrderBook* book : {agent.bybit_book.get(), &agent.venue_book}) {
        book->apply_snapshot(lv("99990.5"), lv("99991"), 1);  // no exchange stamp
        frame(agent, *book);
    }
    OrderBook& spot = agent.reference_book;
    spot.apply_snapshot(lv("99990.5"), lv("99991"), 1, stamp);
    frame(agent, spot);
    spot.apply_bbo(99990.7, 1.0, 99991.0, 1.0, 2);  // a bookTicker: the bid moved, no stamp
    frame(agent, spot);
    std::vector<std::string> lines, heads;
    for (auto& m : log.messages()) {
        std::string label = m.substr(0, m.find(':'));
        if (label == "BINANCE FUTURES" || label == "BINANCE SPOT" || label == "BYBIT" || label == "ARCUS") {
            lines.push_back(m);
            heads.push_back(m.substr(0, m.find(" exchange ")));
        }
    }
    EXPECT_EQ(heads, (std::vector<std::string> {"BINANCE FUTURES: 100000", "BINANCE FUTURES: 100000.1", "BYBIT: 99990.5", "ARCUS: 99990.5",
                                                "BINANCE SPOT: 99990.5", "BINANCE SPOT: 99990.7"}));
    ASSERT_EQ(lines.size(), 6u);
    EXPECT_EQ(lines[0].rfind("BINANCE FUTURES: 100000 exchange " + utc_time_us(stamp) + " now ", 0), 0u) << lines[0];
    EXPECT_TRUE(ends_with(lines[0], " ms)"));
    EXPECT_EQ(lines[1].rfind("BINANCE FUTURES: 100000.1 exchange " + utc_time_us(stamp + 1500) + " now ", 0), 0u) << lines[1];
    EXPECT_TRUE(std::regex_match(lines[3], std::regex(R"(ARCUS: 99990.5 exchange n/a now \d\d:\d\d:\d\d\.\d{6})"))) << lines[3];
    EXPECT_NE(lines[4].find(" exchange " + utc_time_us(stamp) + " "), std::string::npos) << lines[4];
    EXPECT_NE(lines[5].find(" exchange n/a "), std::string::npos) << lines[5];
}

TEST(AgentTobLines, TobLinesOff) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Config config;
    config.agent.tob_lines = false;
    Agent agent(config);
    agent.venue_book.apply_snapshot(lv("99990.5"), lv("99991"), 1);
    frame(agent, agent.venue_book);
    EXPECT_FALSE(any_starts(log.messages(), "ARCUS:"));
}

TEST(AgentTobLines, AtMostALineABookEveryTobLineMsAndTheFirstChangeAtOnce) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Agent agent {Config()};  // tob_line_ms 250 by default
    FakeClock clock;
    agent.tob_clock = clock.fn();
    OrderBook& book = agent.venue_book;
    auto bid = [&](const char* price, double dt) {
        clock.advance(dt);
        book.apply_snapshot(lv(price), lv("100000"), book.seq + 1);
        agent.on_book_update(book);
    };
    bid("99990.0", 0.0);  // the first: at once
    bid("99990.5", 0.1);  // 100 ms later: not written
    bid("99991.0", 0.1);  // 200 ms: not written
    bid("99991.5", 0.1);  // 300 ms: written
    bid("99991.5", 0.5);  // the same price as the last line: nothing
    bid("99992.0", 2.0);  // after a quiet spell: at once
    std::vector<std::string> lines;
    for (const auto& m : log.messages())
        if (starts_with(m, "ARCUS:")) lines.push_back(m.substr(0, m.find(" exchange")));
    EXPECT_EQ(lines, (std::vector<std::string> {"ARCUS: 99990", "ARCUS: 99991.5", "ARCUS: 99992"}));
}
