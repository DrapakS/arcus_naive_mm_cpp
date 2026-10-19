// Ported from tests/test_quotes.py, and the quote line test of test_opens.py.
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <regex>
#include <sstream>

#include "agent.hpp"
#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "strategy_helpers.hpp"
#include "test_util.hpp"
#include "trading/guard.hpp"
#include "trading/opens.hpp"
#include "trading/quotes.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool ends_with_(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// What the quote lines wrote to stdout while it lives.
class Stdout {
public:
    Stdout() {
        set_stdout_capture([this](const std::string& text) { buffer_ += text; });
    }
    ~Stdout() { set_stdout_capture(nullptr); }
    // The lines written since the last call.
    std::vector<std::string> lines() {
        std::vector<std::string> out;
        std::istringstream in(buffer_);
        for (std::string line; std::getline(in, line);) out.push_back(line);
        buffer_.clear();
        return out;
    }
    std::string text() const { return buffer_; }

private:
    std::string buffer_;
};

struct Watch {
    std::unique_ptr<QuoteStrategy> strategy;
    std::unique_ptr<QuoteWatch> w;
};

Watch watch(double markup = 0.001, double tol = 0.0002, double every_s = 0.0, OpenWindows* open_guard = nullptr) {
    Watch r;
    StrategyOptions o = options(markup, tol, 0.001);
    o.open_guard = open_guard;
    r.strategy = std::make_unique<QuoteStrategy>(nullptr, btc_spec(), o);
    r.w = std::make_unique<QuoteWatch>(*r.strategy, every_s);
    return r;
}

// A guard pausing `side`: after a drop for BUY, after a rise for SELL.
std::unique_ptr<PriceGuard> tripped(const std::string& side = "BUY", double pause_s = 10.0) {
    FakeClock clock;
    auto g = std::make_unique<PriceGuard>(1.0, 0.0001, pause_s, clock.fn());
    g->observe(100'000.0);
    clock.advance(1.0);
    g->observe(side == "BUY" ? 99'000.0 : 101'000.0);
    return g;
}

std::string single(std::vector<std::string> lines) {
    EXPECT_EQ(lines.size(), 1u);
    return lines.empty() ? "" : lines[0];
}

}  // namespace

TEST(Quotes, PrintsBothPricesTheStrategyWouldQuote) {
    Stdout out;
    auto [s, w] = watch();
    w->update(view(100'000.0, 99'999.0, 99'999.1));
    std::string line = single(out.lines());
    // buy = fair*(1-markup) rounded down; sell = (fair+tick)*(1+markup) rounded up
    EXPECT_TRUE(std::regex_match(line, std::regex(R"(\d\d:\d\d:\d\d\.\d{3}  buy 99900  sell 100100\.2  fair bid 100000\.00  )"
                                                  R"(arcus 99999\.0/99999\.1  re-place buy, sell)")))
        << line;
}

TEST(Quotes, PrintsOnlyOnChangeAndMarksMovesBeyondTol) {
    Stdout out;
    auto [s, w] = watch(0.001, 0.0002);
    w->update(view(100'000.0, 99'999.0, 99'999.1));
    out.lines();
    w->update(view(100'000.0, 99'999.0, 99'999.1));
    EXPECT_TRUE(out.lines().empty());  // nothing changed
    w->update(view(100'015.0, 99'999.0, 99'999.1));  // buy target 99914.9: 1.5 bp from 99900
    std::string line = single(out.lines());
    EXPECT_TRUE(contains(line, "buy 99914.9") && !contains(line, "re-place")) << line;  // a trading bot would leave its order
    w->update(view(100'030.0, 99'999.0, 99'999.1));  // 3 bp away now
    line = single(out.lines());
    EXPECT_TRUE(ends_with_(line, "re-place buy, sell")) << line;
    EXPECT_TRUE(contains(w->status(), "now buy 99929.9, sell 100130.2; orders would stand at buy 99929.9, sell 100130.2")) << w->status();
    EXPECT_TRUE(contains(w->status(), "placements so far buy 2, sell 2")) << w->status();
}

TEST(Quotes, NoPricePullsBothSides) {
    Stdout out;
    auto [s, w] = watch();
    w->update(view(100'000.0));
    out.lines();
    w->update(view(std::nullopt));
    std::string line = single(out.lines());
    EXPECT_TRUE(contains(line, "buy -  sell -  fair bid -")) << line;
    EXPECT_TRUE(ends_with_(line, "re-place buy pulled, sell pulled")) << line;
}

TEST(Quotes, AfterAPullTheNextPriceIsAPlacementAgain) {
    Stdout out;
    auto [s, w] = watch();
    w->update(view(100'000.0, 99'999.0, 99'999.1));
    w->update(view(std::nullopt));
    EXPECT_TRUE(contains(w->status(), "orders would stand at buy -, sell -; placements so far buy 1, sell 1")) << w->status();
    out.lines();
    w->update(view(100'000.0, 99'999.0, 99'999.1));  // the same prices are back
    std::string line = single(out.lines());
    EXPECT_TRUE(ends_with_(line, "re-place buy, sell")) << line;
    EXPECT_TRUE(contains(w->status(), "placements so far buy 2, sell 2")) << w->status();
}

TEST(Quotes, EachSideMovesOnItsOwn) {
    Stdout out;
    auto [s, w] = watch(0.001, 0.0002);
    w->update(view(100'000.0, 100'200.0, 100'200.1));  // sell held at bid + tick
    out.lines();
    w->update(view(100'030.0, 100'200.0, 100'200.1));  // buy 3 bp up, sell unchanged
    std::string line = single(out.lines());
    EXPECT_TRUE(contains(line, "sell 100200.1") && ends_with_(line, "re-place buy")) << line;
    EXPECT_TRUE(contains(w->status(), "placements so far buy 2, sell 1")) << w->status();
}

TEST(Quotes, QuoteModeTakesAnyOrderSize) {
    QuoteStrategy(nullptr, btc_spec(), options(0.001, 0.0002, 0.00001));  // it never sizes an order
}

TEST(Quotes, TradingShowsTheWorkingOrderInsteadOfRePlaceMarks) {
    Stdout out;
    auto [gw, strategy] = make(0.001, 0.0002);
    QuoteWatch w(*strategy);
    w.update(view(100'000.0));
    std::string line = single(out.lines());
    EXPECT_TRUE(ends_with_(line, "arcus 99990.0/99990.1  no order")) << line;
    strategy->evaluate(view(100'000.0));
    gw->ack();
    w.update(view(100'010.0));  // the target moved, the order has not (yet)
    line = single(out.lines());
    EXPECT_TRUE(contains(line, "buy 99909.9  sell 100110.2")) << line;
    // 99900 is 1.0 bp under its target and 9.0 bp under the Arcus best bid 99990
    EXPECT_TRUE(ends_with_(line, "order BUY 0.001 @ 99900 [open] 1.0 bp off target, 9.0 bp deep")) << line;
    EXPECT_EQ(w.status(), "quote: now buy 99909.9, sell 100110.2");
}

TEST(Quotes, GuardPausePullsTheThreatenedSideOnly) {
    Stdout out;
    auto [s, w] = watch();
    w->update(view(100'000.0, 99'999.0, 99'999.1));
    out.lines();
    auto drop = tripped("BUY");
    s->guards = {drop.get()};  // a drop
    w->update(view(99'990.0, 99'999.0, 99'999.1));
    std::string line = single(out.lines());
    EXPECT_TRUE(ends_with_(line, "re-place buy pulled  drop pause")) << line;  // the sell stays where it is
    drop->until["BUY"] = 0.0;
    w->update(view(99'985.0, 99'999.0, 99'999.1));
    line = single(out.lines());
    EXPECT_TRUE(ends_with_(line, "re-place buy")) << line;  // placed again once the pause is over
    auto rise = tripped("SELL");
    s->guards = {rise.get()};  // a rise
    w->update(view(100'100.0, 99'999.0, 99'999.1));
    line = single(out.lines());
    EXPECT_TRUE(ends_with_(line, "re-place buy, sell pulled  rise pause")) << line;
}

TEST(Quotes, ALateDataPausePullsBothSides) {
    Stdout out;
    auto [s, w] = watch();
    w->update(view(100'000.0, 99'999.0, 99'999.1));
    out.lines();
    LagGuard lag(200.0, 60.0);
    lag.observe(800.0);  // the market data read 800 ms late
    s->guards = {&lag};
    w->update(view(99'990.0, 99'999.0, 99'999.1));
    std::string line = single(out.lines());
    EXPECT_TRUE(ends_with_(line, "re-place buy pulled, sell pulled  late data pause")) << line;  // not a drop and a rise
}

TEST(Quotes, TradingLineSaysWhenTheOrderIsAloneOnTop) {
    Stdout out;
    auto [gw, strategy] = make(0.000005, 0.01);
    QuoteWatch w(*strategy);
    strategy->evaluate(book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 1.0}}));
    gw->ack();
    w.update(book(100'000.0, {{99'990.0, 1.001}}, {{99'990.1, 1.0}}));
    auto lines = out.lines();
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(ends_with_(lines[0], "0.0 bp deep")) << lines[0];  // at the best bid, among others
    w.update(book(100'000.1, {{99'990.0, 0.001}, {99'989.0, 1.0}}, {{99'990.1, 1.0}}));
    lines = out.lines();
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(ends_with_(lines[0], "0.0 bp deep, alone on top")) << lines[0];
}

TEST(Quotes, LopsidedBookIsShownOnTheLine) {
    Stdout out;
    auto [s, w] = watch(0.0001);
    s->imbalance = Imbalance {5, 5.0, 5.0, 0.0, "both"};
    w->update(book(100'000.0, {{99'990.0, 6.0}}, {{99'990.1, 1.0}}));
    auto lines = out.lines();
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(ends_with_(lines[0], "bids 6.0x asks: sell markup x5")) << lines[0];
}

TEST(Quotes, ALopsidedBookShowsOnlyOnASideItWidens) {
    Stdout out;
    auto [s, w] = watch(0.0001);
    s->imbalance = Imbalance {5, 5.0, 5.0, 0.0, "open"};  // buys only
    w->update(book(100'000.0, {{99'990.0, 6.0}}, {{99'990.1, 1.0}}));
    auto lines = out.lines();
    ASSERT_FALSE(lines.empty());
    EXPECT_FALSE(contains(lines[0], "markup x5")) << lines[0];  // thin asks: the sell keeps its markup, nothing to show
    w->update(book(100'000.0, {{99'990.0, 1.0}}, {{99'990.1, 6.0}}));
    lines = out.lines();
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(ends_with_(lines.back(), "asks 6.0x bids: buy markup x5")) << lines.back();
}

TEST(Quotes, QuoteLineMsWritesAtMostOneLineThatOftenAndTheLastPricesAfterIt) {
    // [agent] quote_line_ms (the ML configs): the ML fair moves on every frame. A change within the
    // interval is shown on the first update after it; mode quote still counts every re-place.
    Stdout out;
    auto [s, w] = watch(0.001, 0.0002, 0.25);
    FakeClock now(100.0);
    w->clock = now.fn();
    w->update(view(100'000.0, 99'999.0, 99'999.1));
    EXPECT_EQ(out.lines().size(), 1u);
    now.advance(0.1);
    w->update(view(100'030.0, 99'999.0, 99'999.1));  // 3 bp: a re-place, but too soon for a line
    w->update(view(100'060.0, 99'999.0, 99'999.1));
    EXPECT_TRUE(out.lines().empty());
    EXPECT_TRUE(contains(w->status(), "placements so far buy 3, sell 3")) << w->status();  // counted all the same
    now.advance(0.2);  // 0.3 s since the last line
    w->update(view(100'060.0, 99'999.0, 99'999.1));  // no new change: the held one comes out now
    std::string line = single(out.lines());
    EXPECT_TRUE(contains(line, "buy 99959.9") && ends_with_(line, "re-place buy, sell")) << line;
    w->update(view(100'060.0, 99'999.0, 99'999.1));
    EXPECT_TRUE(out.lines().empty());  // shown already
}

TEST(Quotes, TheQuoteLineSaysWhenAMarketOpenHoldsAdding) {
    using namespace std::chrono;
    double sunday_reopen = static_cast<double>(duration_cast<seconds>((sys_days {year {2026} / 10 / 18} + hours {22}).time_since_epoch()).count());
    FakeClock clock(sunday_reopen);
    OpenWindows w(std::vector<std::string> {"Sun 18:00", "Mon-Thu 18:00"}, "America/New_York", 60.0, 900.0, clock.fn());
    Stdout out;
    auto r = watch(0.001, 0.0002, 0.0, &w);
    r.w->update(view(100'000.0));
    EXPECT_TRUE(contains(out.text(), "market open: no adding")) << out.text();
}

// ------------------------------------------------- the agent over fake exchanges

namespace {

const char* BTC_MARKET = R"({"markets": [{
    "marketId": 1, "marketDisplayName": "BTC-USD", "status": "ONLINE", "tickSize": "0.1", "stepSize": "0.00000001",
    "tickTiers": [{"upToPrice": "500000", "tick": "0.1"}, {"upToPrice": "1000000", "tick": "0.2"}, {"tick": "5"}],
    "minOrderSize": "0.0001", "minOrderNotional": "5", "maxOrderSize": "10000"}]})";

struct AgentRun {
    explicit AgentRun(const std::string& mode)
        : binance(ioc, FakeBinanceParams {.n_steps = 400, .batch = 5, .drop = {}, .seed = 1, .mid = 100'000.0, .interval = 0.002}),
          arcus(ioc, make_arcus_params()),
          futures(ioc, FakeBinanceFuturesParams {.n_tickers = 2'000, .mid = 100'000.0, .seed = 3, .interval = 0.002}),
          bybit(ioc, FakeBybitParams {.n_books = 2'000, .mid = 100'000.0, .seed = 4, .interval = 0.002}) {
        set_loop(ioc);
        // the market spec the trading modes read first (Python monkeypatches fetch_market)
        arcus.on_get("/v1/markets", [](const HttpRequest&) { return HttpReply(200, BTC_MARKET); });
        for (FakeServer* server : std::initializer_list<FakeServer*> {&binance, &arcus, &futures, &bybit}) server->start();
        Config config;
        config.binance.ws_url = binance.ws_url();
        config.binance.rest_url = binance.rest_url();
        config.arcus.ws_url = arcus.ws_url();
        config.arcus.rest_url = arcus.rest_url();
        config.futures.ws_url = futures.ws_url();
        config.futures.connections = 1;
        config.bybit.ws_url = bybit.ws_url();
        config.bybit.connections = 1;
        config.pricing.window = config.pricing.min_samples = 10;
        config.trading.mode = mode;
        agent = std::make_unique<Agent>(config);
        task = Task::spawn(agent->run(), "agent");
    }

    static FakeArcusParams make_arcus_params() {
        FakeArcusParams p;
        p.mid = 99'990.0;
        return p;
    }

    ~AgentRun() {
        agent->stop();
        run_until(ioc, [this] { return task.done(); }, 5.0);
        for (FakeServer* server : std::initializer_list<FakeServer*> {&binance, &arcus, &futures, &bybit}) server->close();
        run_for(ioc, 0.05);
    }

    boost::asio::io_context ioc;
    FakeBinance binance;
    FakeArcus arcus;
    FakeBinanceFutures futures;
    FakeBybit bybit;
    std::unique_ptr<Agent> agent;
    Task task;
};

}  // namespace

TEST(Quotes, AgentInQuoteModePrintsPricesAndPlacesNothing) {
    Stdout out;
    AgentRun run("quote");
    Agent& agent = *run.agent;
    ASSERT_TRUE(run_until(run.ioc, [&] { return agent.quotes && agent.quotes->moves["BUY"] > 0; }, 10.0));
    EXPECT_FALSE(agent.gateway);
    EXPECT_FALSE(agent.strategy);
    auto printed = out.lines();
    ASSERT_FALSE(printed.empty());
    std::regex prices(R"(  buy (\d|-).*  sell (\d|-))");
    for (const auto& line : printed) EXPECT_TRUE(std::regex_search(line, prices)) << line;
    EXPECT_TRUE(std::any_of(printed.begin(), printed.end(), [](const std::string& l) { return contains(l, "binance ") && contains(l, "arcus "); }));
    run_until(run.ioc, [&] {
        auto more = out.lines();
        printed.insert(printed.end(), more.begin(), more.end());
        bool futures = std::any_of(printed.begin(), printed.end(), [](const std::string& l) { return std::regex_search(l, std::regex(R"(  futures \d+\.\d+/\d+\.\d+)")); });
        bool bybit = std::any_of(printed.begin(), printed.end(), [](const std::string& l) { return std::regex_search(l, std::regex(R"(  bybit \d+\.\d+/\d+\.\d+)")); });
        return futures && bybit;
    }, 5.0);
    EXPECT_TRUE(std::any_of(printed.begin(), printed.end(), [](const std::string& l) { return std::regex_search(l, std::regex(R"(  futures \d+\.\d+/\d+\.\d+)")); }));
    EXPECT_TRUE(std::any_of(printed.begin(), printed.end(), [](const std::string& l) { return std::regex_search(l, std::regex(R"(  bybit \d+\.\d+/\d+\.\d+)")); }));
}

TEST(Quotes, AgentTradingPrintsPricesWithItsOrder) {
    Stdout out;
    AgentRun run("paper");
    Agent& agent = *run.agent;
    std::vector<std::string> printed;
    bool shown = run_until(run.ioc, [&] {
        auto more = out.lines();
        printed.insert(printed.end(), more.begin(), more.end());
        return std::any_of(printed.begin(), printed.end(), [](const std::string& l) { return contains(l, "  order BUY 0.001 @ "); });
    }, 10.0);
    ASSERT_TRUE(shown);
    ASSERT_TRUE(agent.quotes);
    EXPECT_EQ(&agent.quotes->strategy, agent.strategy.get());
    std::regex prices(R"(  buy (\d|-).*  sell (\d|-))");
    for (const auto& line : printed) {
        EXPECT_TRUE(std::regex_search(line, prices)) << line;
        EXPECT_TRUE(ends_with_(line, "no order") || contains(line, "  order ")) << line;
    }
}
