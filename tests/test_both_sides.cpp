// Ported from tests/test_both_sides.py (direction = "both"), and the strategy tests of
// test_opens.py ([open_guard] and [open_halt]), test_session.py and test_drift.py.
#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <chrono>

#include "config.hpp"
#include "feed_helpers.hpp"
#include "strategy_helpers.hpp"
#include "test_util.hpp"
#include "trading/guard.hpp"
#include "trading/opens.hpp"
#include "trading/paper.hpp"
#include "trading/session.hpp"
#include "util/pyfmt.hpp"

#include <format>

using namespace nmm;
using namespace nmm::test;

namespace {

using Actions = std::vector<Action>;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool contains(const std::vector<std::string>& lines, const std::string& line) {
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

std::vector<std::string> sides_of(const Actions& actions) {
    std::vector<std::string> out;
    for (const auto& a : actions) out.push_back(a.side);
    return out;
}

// "place BUY", "cancel SELL": what was asked on which side, sorted.
std::vector<std::string> moves(const Actions& actions) {
    std::vector<std::string> out;
    for (const auto& a : actions) out.push_back(a.kind + " " + a.side);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<std::string, Decimal>> side_sizes(const Actions& actions) {
    std::vector<std::pair<std::string, Decimal>> out;
    for (const auto& a : actions) out.emplace_back(a.side, a.size);
    return out;
}

std::string repo_file(const std::string& name) { return std::string(NMM_SOURCE_DIR) + "/" + name; }

Config load(const std::string& name) { return load_config(repo_file(name)); }

// The settings as one text: equal texts, equal configs.
std::string text(const Config& c) { return join(describe(c), "\n"); }

// A two-sided config with its direction, limit, order size and inventory controls set back to the long one's.
Config as_long(Config both_cfg, const Config& long_cfg) {
    TradingConfig& t = both_cfg.trading;
    const TradingConfig& lt = long_cfg.trading;
    t.direction = lt.direction;
    t.max_position_usd = lt.max_position_usd;
    t.skew_open = lt.skew_open;
    t.skew_close = lt.skew_close;
    t.exit_after_s = lt.exit_after_s;
    t.exit_loss = lt.exit_loss;
    t.order_size = lt.order_size;
    t.markup = lt.markup;  // GLD and NVDA two-sided at half from 20 Oct (the long-only configs kept theirs)
    both_cfg.add_guard = long_cfg.add_guard;
    both_cfg.drift = long_cfg.drift;  // the drift markup: on for BTC and ETH so far
    // from 18 Oct BTC and ETH: the faster futures guard and the Arcus take guard
    both_cfg.futures.window_ms = long_cfg.futures.window_ms;
    both_cfg.futures.drawdown = long_cfg.futures.drawdown;
    both_cfg.futures.pause_ms = long_cfg.futures.pause_ms;
    both_cfg.takes = long_cfg.takes;
    both_cfg.portfolio = long_cfg.portfolio;  // the class's lean across the bots (19 Oct): two-sided crypto bots only
    both_cfg.lag.max_pause_ms = long_cfg.lag.max_pause_ms;  // pauses growing while the data stays late: the live ones (20 Oct)
    return both_cfg;
}

double utc(int y, int mo, int d, int h, int mi) {
    using namespace std::chrono;
    auto day = sys_days {year {y} / mo / d};
    return static_cast<double>(duration_cast<seconds>((day + hours {h} + minutes {mi}).time_since_epoch()).count());
}

const double SUNDAY_REOPEN = utc(2026, 10, 18, 22, 0);  // Sunday 18:00 New York (EDT): the gold reopen of 18 Oct

std::unique_ptr<OpenWindows> windows(const FakeClock& clock, std::vector<std::string> opens = {"Sun 18:00", "Mon-Thu 18:00"},
                                     double before_s = 60.0, double after_s = 900.0) {
    return std::make_unique<OpenWindows>(std::move(opens), "America/New_York", before_s, after_s, clock.fn());
}

const double US_OPEN = utc(2026, 10, 19, 13, 30);  // Monday 09:30 New York (EDT)

// [open_halt] as the stock configs have it: from 60 s before to 2 min after the 09:30 open, nothing trades.
std::unique_ptr<OpenWindows> halt(const FakeClock& clock) {
    return std::make_unique<OpenWindows>(std::vector<std::string> {"Mon-Fri 09:30"}, "America/New_York", 60.0, 120.0, clock.fn(),
                                         "no trading: every order pulled, none placed", "", "trading again");
}

double new_york(int y, int mo, int d, int h, int mi) {
    using namespace std::chrono;
    auto zone = locate_zone("America/New_York");
    auto local = local_days {year {y} / mo / d} + hours {h} + minutes {mi};
    return static_cast<double>(duration_cast<seconds>(zone->to_sys(local).time_since_epoch()).count());
}

}  // namespace

TEST(BothSides, FlatItQuotesBothSidesAndAFullSideStopsUntilThePositionComesBack) {
    LogCapture log("naive_mm.strategy");
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [](StrategyOptions& o) { o.close_markup_cut = 0.3; });
    s->evaluate(view(100'000.0));
    // flat: both sides open, the full markup each
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.0099"), A("place", "SELL", "100100.2", "0.0099")}));
    gw->ack("BUY");
    gw->ack("SELL");
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});
    gw->fill("BUY", "0.0099");  // a $989 long: room for $11 more, a buy of that tops it up
    s->evaluate(view(100'000.0));
    // the sell now closes the long, so it takes the cut markup and moves to it
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.00011001"), A("cancel", "SELL", "100100.2", "0.0099")}));
    gw->ack("BUY");
    gw->fill("BUY", "0.00011001");  // at the limit: no more buying
    s->evaluate(view(100'000.0));
    EXPECT_TRUE(contains(log.messages().back(), "no more buying, at the limit of $1000")) << log.messages().back();
    gw->cancelled("SELL");
    s->evaluate(view(100'000.0));
    // (100000.1 * 1.0007) rounded up; the whole long, as it is bigger than order_size
    EXPECT_EQ(actions(*gw), (Actions {A("place", "SELL", "100070.2", "0.01001001")}));
    gw->ack("SELL");
    gw->fill("SELL", "0.0099");  // a $11 long: buying again; the rest of the sell still closes it at the cut markup
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.0099")}));
    EXPECT_TRUE(contains(log.messages().back(), "buying again, under the limit")) << log.messages().back();
}

TEST(BothSides, EachSideIsCutToWhatKeepsThePositionWithinTheLimit) {
    {
        auto [gw, s] = both();
        gw->add_position(D("0.004"));  // a $400 long
        s->evaluate(view(100'000.0));
        // the buy may add $600 at most (1000 / 99900 = 0.01001 BTC in all); the sell sells a whole order
        EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.00601001"), A("place", "SELL", "100100.2", "0.0099")}));
    }
    {
        auto [gw, s] = both(0.001, 0.0002, 0.01);  // orders of $1001 at the sell's price: the sell is cut to $1000 even flat
        s->evaluate(view(100'000.0));
        EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.01"), A("place", "SELL", "100100.2", "0.00998999")}));
    }
    {
        auto [gw, s] = both();
        gw->add_position(D("-0.0095"));  // a $950 short: the sell has room for $50 more
        s->evaluate(view(100'000.0));
        EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.0099"), A("place", "SELL", "100100.2", "0.00048999")}));
    }
    auto [gw, s] = both(0.001, 0.0002, SIZE, 5000.0);  // a higher limit: whole orders until the position is near it
    gw->add_position(D("0.04"));
    s->evaluate(view(100'000.0));
    // the sell closes the whole $4000 long in one order
    EXPECT_EQ(side_sizes(actions(*gw)), (std::vector<std::pair<std::string, Decimal>> {{"BUY", D("0.0099")}, {"SELL", D("0.04")}}));
}

TEST(BothSides, ASideHeldAtTheLimitComesBackOnceThePositionIsUnder95PercentOfIt) {
    {
        auto [gw, s] = both();
        gw->add_position(D("0.01"));  // $999 at 99900: no valid buy fits
        s->evaluate(view(100'000.0));
        EXPECT_EQ(sides_of(actions(*gw)), (std::vector<std::string> {"SELL"}));
        gw->add_position(D("-0.00045"));  // a $954 long: room for a buy, but still over 95% of the limit
        s->evaluate(view(100'000.0));
        EXPECT_EQ(actions(*gw), Actions {});  // (the sell of 0.01 is 0.0001 over the 0.0099 wanted: left alone)
        gw->add_position(D("-0.0001"));  // a $944 long: under $950, buying again with the room left
        s->evaluate(view(100'000.0));
        EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.00056001")}));
    }
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [](StrategyOptions& o) { o.max_position_resume = 0.5; });  // back only under $500
    gw->add_position(D("0.01"));
    s->evaluate(view(100'000.0));
    actions(*gw);
    gw->add_position(D("-0.0046"));  // $539
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});
    gw->add_position(D("-0.0005"));  // $489
    s->evaluate(view(100'000.0));
    EXPECT_EQ(sides_of(actions(*gw)), (std::vector<std::string> {"BUY"}));
}

TEST(BothSides, AWorkingOrderIsPulledWhenItsSideReachesTheLimit) {
    auto [gw, s] = both();
    s->evaluate(view(100'000.0));
    actions(*gw);
    gw->ack("BUY");
    gw->ack("SELL");
    gw->add_position(D("-0.0099"));  // a short from elsewhere (a stray filled): the sell has no room now
    s->check_cancel(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "SELL", "100100.2", "0.0099")}));
    EXPECT_EQ(gw->why, "position -0.0099 is at the limit of $1000");
}

TEST(BothSides, EachSideWaitsOnlyForItsOwnAnswers) {
    auto [gw, s] = both();
    s->evaluate(view(100'000.0));
    actions(*gw);
    gw->ack("BUY");
    gw->ack("SELL");
    s->pull("SELL", "rise: a guard");
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "SELL", "100100.2", "0.0099")}));
    s->evaluate(view(100'030.0));  // the buy's target moved 3 bp: it goes, the sell's cancel still in flight
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.0099")}));
    gw->cancelled("BUY");
    s->evaluate(view(100'030.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99929.9", "0.0099")}));  // placed, the sell still pending cancel
    EXPECT_EQ(gw->orders.at("SELL")->state, OrderState::PENDING_CANCEL);
}

TEST(BothSides, GuardsHoldEachSideOnItsOwn) {
    FakeClock clock;
    PriceGuard guard(1.0, 0.0001, 0.5, clock.fn());
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.guards = {&guard}; });
    for (int i = 0; i < 2; ++i) {
        guard.observe(100'000.0);
        clock.advance(0.6);
    }
    auto trip = guard.observe(99'980.0);  // a 2 bp drop: buys held
    ASSERT_TRUE(trip);
    EXPECT_TRUE(trip->drop());
    s->evaluate(view(100'000.0));
    EXPECT_EQ(sides_of(actions(*gw)), (std::vector<std::string> {"SELL"}));  // the sell is placed all the same
}

TEST(BothSides, TheTrendGuardHoldsBuysOnAFallAndSellsOnARally) {
    FakeClock clock;
    TrendGuard g(120.0, 0.003, 60.0, "both", clock.fn());
    EXPECT_FALSE(g.observe(100'000.0));
    clock.advance(10);
    auto trip = g.observe(99'650.0);  // 35 bp under the high
    ASSERT_TRUE(trip);
    EXPECT_EQ(trip->side, "BUY");
    EXPECT_TRUE(g.paused("BUY"));
    EXPECT_FALSE(g.paused("SELL"));
    EXPECT_EQ(g.reference_of("BUY"), "high");
    clock.advance(10);
    trip = g.observe(100'000.0);  // 35 bp over the low
    ASSERT_TRUE(trip);
    EXPECT_EQ(trip->side, "SELL");
    EXPECT_TRUE(g.paused("SELL"));
    EXPECT_EQ(g.reference_of("SELL"), "low");
    TrendGuard g2(120.0, 0.003, 60.0, "both", clock.fn());
    g2.observe(100'000.0);
    clock.advance(1);
    g2.observe(99'000.0);
    clock.advance(1);
    g2.observe(99'650.0);  // far under the high and far over the low at once: both held
    EXPECT_TRUE(g2.paused("BUY"));
    EXPECT_TRUE(g2.paused("SELL"));
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.guards = {&g2}; });
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), Actions {});
}

TEST(BothSides, PaperFillsEachSideAgainstTheTradesThatCrossIt) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    PaperGateway gw(btc_spec(), "ws://unused");
    gw.ready = true;
    auto buy = gw.place("BUY", Decimal(99900), D("0.001"));
    auto sell = gw.place("SELL", Decimal(100100), D("0.001"));
    EXPECT_EQ(gw.working(), (std::vector<OrderPtr> {buy, sell}));
    auto trades = [](std::vector<std::pair<const char*, const char*>> rows) {
        JsonArray contents;
        for (auto [p, s] : rows) contents.push_back(JsonObject {{"price", p}, {"size", s}});
        return Json(JsonObject {{"type", "channel_data"}, {"channel", "trades"}, {"id", "BTC-USD"}, {"contents", contents}});
    };
    gw.on_message(trades({{"100000", "1"}}));  // between them: neither
    EXPECT_EQ(buy->filled, 0);
    EXPECT_EQ(sell->filled, 0);
    gw.on_message(trades({{"100150", "0.0004"}, {"99850", "0.002"}}));
    EXPECT_EQ(sell->filled, D("0.0004"));
    EXPECT_EQ(buy->state, OrderState::FILLED);
    EXPECT_EQ(gw.position(), D("0.0006"));
    EXPECT_EQ(gw.working(), (std::vector<OrderPtr> {sell}));
    gw.cancel(sell);
    run_for(ioc, 0.05);
    EXPECT_TRUE(gw.working().empty());
    EXPECT_EQ(sell->state, OrderState::CANCELED);
}

TEST(BothSides, TheShippedTwoSidedConfigsAreTheirLongOnesButForTheDirectionLimitAndInventoryControls) {
    // BTC and ETH from 18 Oct afternoon: ~$750 orders within $1500; SOL and GLD: $3000
    struct Case {
        const char* coin;
        const char* long_name;
        double limit;
    };
    for (auto [coin, long_name, limit] : {Case {"btc", "recommended", 1500.0}, Case {"eth", "eth", 1500.0}, Case {"sol", "sol", 3000.0},
                                          Case {"gld", "gld", 3000.0}}) {
        Config both_cfg = load(std::format("config.{}-both.toml", coin));
        Config long_cfg = load(std::format("config.{}.toml", long_name));
        const TradingConfig& t = both_cfg.trading;
        EXPECT_EQ(t.direction, "both") << coin;
        EXPECT_EQ(t.max_position_usd, limit) << coin;
        EXPECT_EQ(t.skew_open, 1) << coin;
        EXPECT_EQ(t.skew_close, 1) << coin;
        EXPECT_EQ(t.exit_after_s, 0) << coin;  // no urgent exit from 18 Oct
        EXPECT_EQ(t.exit_loss, 0) << coin;
        if (std::string(coin) == "btc" || std::string(coin) == "eth") {  // half the limit an order
            double usd = t.order_size * (std::string(coin) == "btc" ? 81'600 : 2'405);
            EXPECT_TRUE(730 < usd && usd < 770) << coin << " " << usd;
        }
        EXPECT_GT(both_cfg.add_guard.drawdown, 0) << coin;
        EXPECT_EQ(both_cfg.add_guard.limit_share, 0.5) << coin;
        EXPECT_EQ(text(as_long(both_cfg, long_cfg)), text(long_cfg)) << coin;
    }
}

TEST(BothSides, TheShippedNvdaConfigsSizeAndLimitAndDoubleTheMarkupOffHours) {
    Config both_cfg = load("config.nvda-both.toml"), long_cfg = load("config.nvda.toml");
    EXPECT_EQ(both_cfg.trading.direction, "both");
    EXPECT_EQ(both_cfg.trading.max_position_usd, 2000.0);  // 20 Oct, the owner's
    double both_usd = both_cfg.trading.order_size * 217;  // ~$1000 orders (20 Oct; were $500)
    EXPECT_TRUE(980 < both_usd && both_usd < 1020) << both_usd;
    double usd = long_cfg.trading.order_size * 214;  // the long-only one as it was: ~$500
    EXPECT_TRUE(480 < usd && usd < 500) << usd;
    EXPECT_EQ(both_cfg.trading.off_hours_markup_factor, 2.0);
    EXPECT_EQ(long_cfg.trading.off_hours_markup_factor, 2.0);
    EXPECT_EQ(load("config.gld.toml").trading.off_hours_markup_factor, 1.0);
    const TradingConfig& t = both_cfg.trading;
    EXPECT_EQ(t.skew_open, 1);
    EXPECT_EQ(t.skew_close, 1);
    EXPECT_EQ(t.exit_after_s, 0);
    EXPECT_EQ(t.exit_loss, 0);
    EXPECT_EQ(both_cfg.add_guard.drawdown, 0);
    EXPECT_EQ(text(as_long(both_cfg, long_cfg)), text(long_cfg));
}

TEST(BothSides, TheTwoSidedMlConfigIsTheMlOneWithTheTwoSidedBtcControls) {
    // C++ checkout: config.btc-ml-both.toml is the live BTC ML set, config.btc-ml-okx-only.toml as it is
    // (Python's is the ML model with config.btc-both.toml's controls); the model the BTC 10 s refit of 20 Oct
    Config ml_both = load("config.btc-ml-both.toml"), ml_long = load("config.btc-ml.toml"), live = load("config.btc-ml-okx-only.toml");
    Config same = ml_both;
    same.trading.markup = live.trading.markup;  // 0.1 bp here from 20 Oct
    same.trading.order_size = live.trading.order_size;  // ~$2000 within $3750 here from 20 Oct
    same.trading.max_position_usd = live.trading.max_position_usd;
    same.ml = live.ml;  // the 10 s refit here from 20 Oct
    same.lag = live.lag;  // on here from 20 Oct, its pauses growing while the data stays late
    EXPECT_EQ(text(same), text(live));
    EXPECT_EQ(ml_both.ml.target_window_s, 10.0);
    EXPECT_EQ(ml_both.pricing.model, "ml");
    EXPECT_EQ(ml_both.trading.direction, "both");
    EXPECT_EQ(ml_long.ml.target_window_s, 30.0);  // the long ML config keeps the 30 s model of 19 Oct
    // ETH (18 Oct night): config.eth-both.toml as it is, only the pricing is the model
    Config eth_ml = load("config.eth-ml-both.toml"), eth = load("config.eth-both.toml");
    EXPECT_EQ(eth_ml.pricing.model, "ml");
    EXPECT_EQ(eth.pricing.model, "basis");
    EXPECT_EQ(eth_ml.agent.quote_line_ms, 1000);  // the ML fair moves on every frame
    EXPECT_EQ(eth.agent.quote_line_ms, 0);
    // ML bots read Binance spot trades, Bybit and OKX over one connection each (19 Oct: loop stalls), basis over two
    for (const Config* c : {&eth_ml, &eth}) {
        int n = c == &eth_ml ? 1 : 2;
        EXPECT_EQ(c->futures.connections, n);
        EXPECT_EQ(c->trades.connections, n);
        EXPECT_EQ(c->bybit.connections, n);
        EXPECT_EQ(c->okx.connections, n);
    }
    EXPECT_EQ(eth_ml.trading.tol_away, 0.000075);  // ML: 0.75 bp (19 Oct)
    EXPECT_EQ(eth.trading.tol_away, 0.00015);
    // From 19 Oct evening the live ETH ML config goes its own way (test_live_19oct.cpp); the rest as eth-both
    auto section = [](const Config& c, const std::string& name) {
        for (const auto& line : describe(c))
            if (line.starts_with("[" + name + "] ")) return line;
        return std::string();
    };
    for (std::string name : {"arcus", "imbalance", "open_guard", "open_halt", "vol_markup"})
        EXPECT_EQ(section(eth_ml, name), section(eth, name)) << name;
    // the Arcus book over two even on ML (19 Oct, evening): a late connection shows against the other one's copies
    EXPECT_EQ(eth_ml.binance.connections, 1);
    EXPECT_EQ(eth_ml.arcus.connections, 2);
}

TEST(BothSides, TheQuoteLinesTakeTheLoopsPricesAndACancelCheckPricesOnlySidesWithAnOrder) {
    auto [gw, s] = both();
    MarketView loop_view = view(100'000.0);
    s->evaluate(loop_view);
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99900.0", "0.0099"), A("place", "SELL", "100100.2", "0.0099")}));
    // The quote lines of the same loop: the prices the strategy just placed at, not worked out again.
    // (Python checks that price() is not called; here a markup changed after the loop shows it.)
    s->markup = D("0.002");
    s->close_markup = D("0.002");
    std::map<std::string, std::optional<Decimal>> placed {{"BUY", D("99900.0")}, {"SELL", D("100100.2")}};
    EXPECT_EQ(s->prices(loop_view), placed);
    MarketView other = view(100'000.0);
    EXPECT_NE(s->prices(other), placed);  // another view: priced now
    s->markup = D("0.001");
    s->close_markup = D("0.001");
    EXPECT_EQ(s->prices(other), placed);
    gw->ack("BUY");
    gw->cancelled("SELL");  // only the buy works now
    s->check_cancel(view(100'100.0));  // the buy is 1 bp under its target: moved
    EXPECT_EQ(actions(*gw), (Actions {A("cancel", "BUY", "99900.0", "0.0099")}));
}

// ------------------------------------------------------------- from test_opens.py

TEST(BothSides, AroundAnOpenNothingOpensAndTheClosingOrderDoesNotGoThroughZero) {
    // GLD, 18 Oct 22:03-22:08 UTC: seven sells in a row into the Sunday gold reopen, 4 GLD short, -31 bp.
    FakeClock clock(SUNDAY_REOPEN - 3600);
    auto w = windows(clock);
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.open_guard = w.get(); });
    s->evaluate(view(100'000.0));
    EXPECT_EQ(sides_of(actions(*gw)), (std::vector<std::string> {"BUY", "SELL"}));  // an hour before: as usual
    gw->ack("BUY");
    gw->ack("SELL");
    gw->fill("BUY", "0.004");  // a small long
    clock.set(SUNDAY_REOPEN + 120);
    s->evaluate(view(100'000.0));
    auto sent = actions(*gw);
    auto has = [&](const char* kind, const char* side) {
        return std::any_of(sent.begin(), sent.end(), [&](const Action& a) { return a.kind == kind && a.side == side; });
    };
    EXPECT_TRUE(has("cancel", "BUY"));  // adding to the long: pulled
    EXPECT_TRUE(has("cancel", "SELL"));  // a whole order: it would go short past zero
    gw->orders.clear();
    s->evaluate(view(100'000.0));
    EXPECT_EQ(actions(*gw), (Actions {A("place", "SELL", "100100.2", "0.004")}));
    clock.set(SUNDAY_REOPEN + 901);  // the window is over: buying again (to the $1000 limit), the closing order whole
    gw->orders.clear();
    s->evaluate(view(100'000.0));
    EXPECT_EQ(side_sizes(actions(*gw)), (std::vector<std::pair<std::string, Decimal>> {{"BUY", D("0.00601001")}, {"SELL", D("0.0099")}}));
}

TEST(BothSides, FlatWithinTheWindowNothingIsQuotedAndALimitShareLetsASmallPositionOpen) {
    FakeClock clock(SUNDAY_REOPEN);
    auto w = windows(clock);
    {
        auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.open_guard = w.get(); });
        s->evaluate(view(100'000.0));
        EXPECT_EQ(actions(*gw), Actions {});
    }
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) {  // up to $250 either way
        o.open_guard = w.get();
        o.open_limit_share = 0.25;
    });
    s->evaluate(view(100'000.0));
    EXPECT_EQ(side_sizes(actions(*gw)), (std::vector<std::pair<std::string, Decimal>> {{"BUY", D("0.00250250")}, {"SELL", D("0.00249749")}}));
}

TEST(BothSides, OneSidedLongAroundAnOpenBuysNothingButSellsWhatItHolds) {
    FakeClock clock(SUNDAY_REOPEN);
    auto w = windows(clock);
    FakeGateway gw;
    StrategyOptions o = options(0.001, 0.0002, 0.001);
    o.open_guard = w.get();
    QuoteStrategy s(&gw, btc_spec(), o);
    s.evaluate(view(100'000.0));
    EXPECT_EQ(actions(gw), Actions {});  // flat: the buy would open a long
    gw.set_position(D("0.001"));
    s.evaluate(view(100'000.0));
    auto sent = actions(gw);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].kind + " " + sent[0].side, "place SELL");
}

TEST(BothSides, AtTheUsStockOpenNothingTradesTheClosingOrderTooThenTradingAgain) {
    // NVDA, 19 Oct 13:30 UTC: -50 bp in the first 30 s of the open; its closing sell was hit at 217.15
    // against a 218.54 buy. From 60 s before to 2 min after the open every order is pulled, none placed.
    LogCapture log(ORDERS_LOGGER);
    FakeClock clock(US_OPEN - 120);
    auto w = halt(clock);
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.halt = w.get(); });
    s->evaluate(view(100'000.0));
    EXPECT_EQ(moves(actions(*gw)), (std::vector<std::string> {"place BUY", "place SELL"}));  // 2 min before: as usual
    gw->ack("BUY");
    gw->ack("SELL");
    gw->fill("BUY", "0.004");  // a long: its closing sell is working
    clock.set(US_OPEN - 59);
    s->check_cancel(view(100'000.0));  // the per-frame check pulls both at once, the closing one too
    EXPECT_EQ(moves(actions(*gw)), (std::vector<std::string> {"cancel BUY", "cancel SELL"}));
    EXPECT_EQ(gw->why, "market open: no trading");
    EXPECT_TRUE(contains(log.messages(), "--- market open Mon-Fri 09:30 America/New_York (Mon 13:30 UTC): no trading: every order "
                                         "pulled, none placed until Mon 13:32 UTC"));
    gw->orders.clear();
    for (double at : {US_OPEN, US_OPEN + 119}) {
        clock.set(at);
        s->evaluate(view(100'000.0));
        EXPECT_EQ(actions(*gw), Actions {}) << at - US_OPEN;
    }
    clock.set(US_OPEN + 121);  // over: both sides again
    s->evaluate(view(100'000.0));
    EXPECT_EQ(moves(actions(*gw)), (std::vector<std::string> {"place BUY", "place SELL"}));
    EXPECT_TRUE(contains(log.messages(), "--- market open window over: trading again"));
}

TEST(BothSides, OneSidedTheHaltPullsTheSellOfWhatItHoldsToo) {
    FakeClock clock(US_OPEN + 30);
    auto w = halt(clock);
    FakeGateway gw;
    gw.set_position(D("0.001"));
    StrategyOptions o = options(0.001, 0.0002, 0.001);
    o.halt = w.get();
    QuoteStrategy s(&gw, btc_spec(), o);
    s.evaluate(view(100'000.0));
    EXPECT_EQ(actions(gw), Actions {});
}

// ------------------------------------------------------------- from test_session.py

TEST(BothSides, OutsideTheSessionBothSidesTakeTwiceTheMarkup) {
    FakeClock now(new_york(2026, 10, 16, 12, 0));
    // NVDA-USD's hours as Arcus lists them: 04:00-20:00 New York
    Json rth = parse_json(R"({"startSecondsOfDay": 14400, "endSecondsOfDay": 72000, "timezone": "America/New_York", "isOvernight": false})");
    SessionClock session(*Hours::from_json(&rth), false, now.fn());
    auto quotes = [&] {
        FakeGateway gw;
        StrategyOptions o = options(0.001, 0.0002, 0.001);
        o.direction = "both";
        o.session = &session;
        o.off_hours_markup_factor = 2.0;
        QuoteStrategy s(&gw, btc_spec(), o);
        s.evaluate(view(100'000.0));
        return actions(gw);
    };
    EXPECT_EQ(quotes(), (Actions {A("place", "BUY", "99900.0", "0.001"), A("place", "SELL", "100100.2", "0.001")}));
    now.advance(9 * 3600);  // 21:00: closed
    EXPECT_EQ(quotes(), (Actions {A("place", "BUY", "99800.0", "0.001"), A("place", "SELL", "100200.2", "0.001")}));
}

// ------------------------------------------------------------- from test_drift.py

TEST(BothSides, ADriftedSideTakesMarkupTimesTheFactorAndThePlacementSaysSo) {
    LogCapture log("naive_mm.orders");
    FakeClock clock(0.0);
    DriftWatch watch(10.0, 0.00002, 2.0, clock.fn());
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) {
        o.drift = &watch;
        o.drift_factor = 1.5;
    });
    watch.observe(100'000.0);
    clock.advance(10.0);
    watch.observe(99'990.0);  // -1 bp: buys drifted
    s->evaluate(view(100'000.0));
    // the buy 15 bp under the fair (1.5 x 10 bp), the sell at its own 10 bp over
    EXPECT_EQ(actions(*gw), (Actions {A("place", "BUY", "99850.0", "0.0099"), A("place", "SELL", "100100.2", "0.0099")}));
    std::vector<std::string> placed;
    for (const auto& m : log.messages())
        if (m.starts_with(">>> PLACE")) placed.push_back(m);
    ASSERT_EQ(placed.size(), 2u);
    EXPECT_TRUE(contains(placed[0], "drift -1.00 bp in 10 s for it, markup x1.5")) << placed[0];
    EXPECT_TRUE(contains(placed[1], "drift +1.00 bp in 10 s for it")) << placed[1];
    EXPECT_FALSE(contains(placed[1], "markup x"));
}

TEST(BothSides, WithTheFactorAt1TheDriftIsOnlyLogged) {
    FakeClock clock(0.0);
    DriftWatch watch(10.0, 0.00002, 2.0, clock.fn());
    auto [gw, s] = both(0.001, 0.0002, SIZE, 1000.0, [&](StrategyOptions& o) { o.drift = &watch; });
    watch.observe(100'000.0);
    clock.advance(10.0);
    watch.observe(99'990.0);
    s->evaluate(view(100'000.0));
    auto sent = actions(*gw);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].price, D("99900.0"));
    EXPECT_EQ(sent[1].price, D("100100.2"));
    EXPECT_EQ(s->drift_note("BUY"), "drift -1.00 bp in 10 s for it");
}
