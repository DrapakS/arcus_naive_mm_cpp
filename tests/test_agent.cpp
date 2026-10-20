// Ported from tests/test_agent.py: the agent's books, pricing, guards and cancel checks, and the config checks there.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <cmath>
#include <regex>

#include "agent_helpers.hpp"
#include "fake_server.hpp"
#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

// Run agent.run() on the loop until stop(); returns whether it finished in time.
struct Running {
    Agent& agent;
    boost::asio::io_context& ioc;
    bool finished = false;
    std::exception_ptr error;
    Running(Agent& a, boost::asio::io_context& io) : agent(a), ioc(io) {
        boost::asio::co_spawn(ioc, agent.run(), [this](std::exception_ptr e) {
            error = e;
            finished = true;
        });
    }
    ~Running() {
        agent.stop();
        run_until(ioc, [this] { return finished; }, 5.0);
    }
};

}  // namespace

TEST(Agent, TracksBothBooksAndWakesStrategy) {
    Loop loop;
    FakeBinance binance(loop.ioc, {.n_steps = 200});
    FakeArcus arcus(loop.ioc, {.n_events = 200});
    binance.start();
    arcus.start();
    Config config;
    config.binance.ws_url = binance.ws_url();
    config.binance.rest_url = binance.rest_url();
    config.arcus.ws_url = arcus.ws_url();
    config.arcus.rest_url = arcus.rest_url();
    config.futures.enabled = false;  // covered in the futures tests
    config.bybit.enabled = false;  // covered in the Bybit tests
    config.trades.enabled = false;
    config.agent.status_interval_s = 0.05;
    config.pricing.min_samples = 10;
    Agent agent(config);
    {
        Running running(agent, loop.ioc);
        ASSERT_TRUE(run_until(loop.ioc, [&] { return agent.view->reference && agent.view->venue; }, 10));
        auto view = agent.view;
        EXPECT_EQ(view->reference->bids.size(), 1u);  // of spot only the top: nothing reads more
        EXPECT_LE(view->venue->asks.size(), 10u);
        EXPECT_TRUE(view->basis_bps().has_value());
        ASSERT_TRUE(run_until(loop.ioc, [&] { return binance.done && arcus.done; }, 10));
        ASSERT_TRUE(run_until(loop.ioc, [&] {
            return agent.reference_book.seq == binance.update_id && agent.venue_book.seq == arcus.seq && agent.view->reference &&
                   agent.view->venue && agent.view->reference->seq == binance.update_id && agent.view->venue->seq == arcus.seq;
        }));
        auto last = agent.view;
        auto [true_bids, true_asks] = binance.book.as_floats();
        ASSERT_FALSE(true_bids.empty());
        EXPECT_EQ(last->reference->bids, (std::vector<Level> {true_bids[0]}));
        auto [arcus_bids, arcus_asks] = arcus.book.as_floats();
        std::vector<Level> want(arcus_asks.begin(), arcus_asks.begin() + std::min<size_t>(10, arcus_asks.size()));
        EXPECT_EQ(last->venue->asks, want);
        EXPECT_NE(format_ladders(*last->reference, *last->venue).find("binance BTCUSDT"), std::string::npos);
        EXPECT_GE(agent.pricer->samples(), 10u);
        ASSERT_TRUE(last->fair_bid.has_value());
        EXPECT_NEAR(*last->fair_bid, *last->reference->best_bid() * *agent.pricer->ratio(), 1e-6);
    }
}

TEST(Agent, FairBidSampledOnArcusUpdatesAndPricedOnEveryUpdate) {
    Loop loop;
    Config config;
    config.pricing.window = 3;
    config.pricing.min_samples = 2;
    Agent agent(config);
    auto& ref = agent.reference_book;
    auto& venue = agent.venue_book;

    venue.apply_snapshot(lv("101"), lv("101.5"), 1);
    frame(agent, venue);  // Binance not ready yet: no sample, no price
    EXPECT_EQ(agent.pricer->samples(), 0u);
    EXPECT_FALSE(agent.fair_bid);

    ref.apply_snapshot(lv("100"), lv("100.1"), 1);
    frame(agent, ref);  // Binance updates never add samples
    EXPECT_EQ(agent.pricer->samples(), 0u);
    frame(agent, venue);  // sample 101 / 100
    EXPECT_EQ(agent.pricer->samples(), 1u);
    EXPECT_FALSE(agent.fair_bid);  // warming up

    ref.apply_update(parse_json(R"([["100", "0"], ["200", "1"]])"), parse_json(R"([["100.1", "0"], ["200.1", "1"]])"), 2);
    frame(agent, ref);
    EXPECT_EQ(agent.pricer->samples(), 1u);
    EXPECT_FALSE(agent.fair_bid);
    venue.apply_update(parse_json(R"([["101", "0"], ["204", "1"]])"), parse_json(R"([["101.5", "0"], ["204.5", "1"]])"), 2);
    frame(agent, venue);  // sample 204 / 200
    ASSERT_TRUE(agent.fair_bid);
    EXPECT_NEAR(*agent.fair_bid, 200 * (1.01 + 1.02) / 2, 1e-6);

    ref.apply_update(parse_json(R"([["200", "0"], ["210", "1"]])"), parse_json(R"([["200.1", "0"], ["210.1", "1"]])"), 3);
    frame(agent, ref);  // Binance moved: same ratio, new estimate
    EXPECT_EQ(agent.pricer->samples(), 2u);
    EXPECT_NEAR(*agent.fair_bid, 210 * 1.015, 1e-6);

    ref.invalidate();
    frame(agent, ref);
    EXPECT_FALSE(agent.fair_bid);
}

TEST(Agent, LaddersPrintedOnEveryBookUpdate) {
    Loop loop;
    std::string out;
    set_stdout_capture([&out](const std::string& text) { out += text; });
    Agent agent(Config(), true);
    agent.reference_book.apply_snapshot(lv("100"), lv("100.1"), 1);
    frame(agent, agent.reference_book);
    agent.venue_book.apply_snapshot(lv("100.05", "2"), lv("100.15", "2"), 7);
    frame(agent, agent.venue_book);
    agent.venue_book.apply_update(parse_json(R"([["100.05", "0"], ["100", "3"]])"), JsonArray {}, 8);
    frame(agent, agent.venue_book);
    agent.reference_book.apply_bbo(100.02, 4.0, 100.08, 5.0, 5);
    frame(agent, agent.reference_book);
    set_stdout_capture(nullptr);

    std::vector<std::string> blocks;
    std::string text(strip(out));
    for (size_t start = 0;;) {
        size_t end = text.find("\n\n", start);
        blocks.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 2;
    }
    ASSERT_EQ(blocks.size(), 4u) << out;
    auto has = [](const std::string& block, const std::string& needle) { return block.find(needle) != std::string::npos; };
    EXPECT_TRUE(has(blocks[0], "binance OK  seq 1") && has(blocks[0], "arcus BTC-USD [SYNCING]")) << blocks[0];
    EXPECT_TRUE(has(blocks[1], "arcus OK  seq 7") && has(blocks[1], "basis")) << blocks[1];
    EXPECT_TRUE(has(blocks[2], "arcus OK  seq 8")) << blocks[2];
    EXPECT_TRUE(has(blocks[1], "100.05") && !has(blocks[2], "100.05"));
    EXPECT_TRUE(has(blocks[3], "binance OK  seq 1  bbo +4") && has(blocks[3], "100.02")) << blocks[3];
}

TEST(Agent, ConfigOverridesAndValidation) {
    TempFile f;
    f.write("[arcus]\nn_levels = 50\nws_url = \"wss://api.testnet.arcus.xyz/v1/ws\"\n[agent]\ndepth = 15\n");
    Config config = load_config(f.written);
    EXPECT_EQ(config.arcus.n_levels, 50);
    EXPECT_EQ(config.agent.depth, 15);
    EXPECT_EQ(config.binance.symbol, "BTCUSDT");

    f.write("[arcus]\nnlevels = 50\n");
    EXPECT_LOAD_ERROR(f.written, {}, "unknown config key: arcus.nlevels");
    f.write("[arcus]\nn_levels = 5\n");
    EXPECT_LOAD_ERROR(f.written, {}, "agent.depth");
    f.write("[binance]\nbook_ticker = false\nstale_timeout_s = 10\n");
    config = load_config(f.written);
    EXPECT_FALSE(config.binance.book_ticker);
    EXPECT_EQ(config.binance.stale_timeout_s, 10.0);
    f.write("[binance]\nbook_ticker = \"false\"\n");
    EXPECT_LOAD_ERROR(f.written, {}, "binance.book_ticker must be bool");
    for (const char* mode : {"off", "quote", "paper", "live"}) {
        f.write(std::format("[trading]\nmode = \"{}\"\n", mode));
        EXPECT_EQ(load_config(f.written).trading.mode, mode);
    }
    f.write("[trading]\nmode = \"dry\"\n");
    EXPECT_LOAD_ERROR(f.written, {}, "trading.mode");
}

class BookFrameCancels : public ::testing::TestWithParam<bool> {};

TEST_P(BookFrameCancels, TheWorkingOrderOnTheSpot) {
    bool acked = GetParam();
    Loop loop;
    Config config;
    config.pricing.window = config.pricing.min_samples = 1;
    Agent agent(config);
    FakeGateway* gw = install(agent, opts(0.001, 0.0002));
    agent.reference_book.apply_snapshot(lv("100000"), lv("100000.1"), 1);
    agent.venue_book.apply_snapshot(lv("99990"), lv("99990.1"), 1);
    frame(agent, agent.venue_book);  // one ratio sample: fair bid 99990
    evaluate(agent);
    if (acked) gw->ack();
    EXPECT_EQ(gw->order()->state, acked ? OrderState::OPEN : OrderState::PENDING_NEW);
    ASSERT_EQ(gw->sent.size(), 1u);
    EXPECT_EQ(gw->sent[0].first, "place");
    EXPECT_EQ(gw->sent[0].second->side, "BUY");
    EXPECT_EQ(gw->sent[0].second->price, Decimal("99890.0"));

    agent.reference_book.apply_update(lv("100050"), JsonArray {}, 2);  // Binance moves up 5 bp
    frame(agent, agent.reference_book);  // the strategy loop never ran
    // cancelled on this frame, whether or not the exchange has acknowledged the order yet
    EXPECT_EQ(gw->kinds(), (Kinds {"place", "cancel"}));
}

INSTANTIATE_TEST_SUITE_P(Acked, BookFrameCancels, ::testing::Values(true, false));

TEST(Agent, BinanceDropPullsABuyAndHoldsOffBuying) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = guarded_agent("BUY");
    Agent& agent = *b.agent;
    top(agent, agent.reference_book, "100020", 4);  // a rise, 2 bp over the mean: no threat to a buy
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    top(agent, agent.reference_book, "99985", 5);  // a drop, 1.5 bp under the mean: 100020 stood for no time
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));  // pulled on the spot
    EXPECT_TRUE(contains(log.messages(),
                         "!!! DROP Binance bid 99985.00 is 1.50 bp under its mean 100000.00 over the last 10 ms: no buying for 50 ms"))
        << join(log.messages(), "\n");
    b.gw->confirm_cancel();
    A::changed(agent).clear();
    evaluate(agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));  // no buying during the pause
    EXPECT_TRUE(A::resume_timers(agent).count({agent.guard.get(), "BUY"}));
    b.clock.advance(0.06);  // the guards' clock: the pause is over by the time the timer fires
    loop.run_for(0.08);
    EXPECT_TRUE(A::resume_timers(agent).empty());
    EXPECT_TRUE(A::changed(agent).is_set());  // woken once each pause is over
    EXPECT_TRUE(contains(log.messages(), "--- Binance buying pause over: 50 ms, extended 0 times"));
    evaluate(agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel", "place"}));
}

TEST(Agent, BinanceRisePullsASellAndLeavesBuysAlone) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = guarded_agent("SELL");
    Agent& agent = *b.agent;
    top(agent, agent.reference_book, "99980", 4);  // a drop, 2 bp under the mean: no threat to a sell
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
    b.clock.advance(0.005);  // 99980 stands for half the window
    top(agent, agent.reference_book, "100030", 5);  // a rise, 4 bp over the mean 99990
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
    EXPECT_TRUE(contains(log.messages(),
                         "!!! RISE Binance bid 100030.00 is 4.00 bp over its mean 99990.00 over the last 10 ms: no selling for 50 ms"))
        << join(log.messages(), "\n");
}

TEST(Agent, CancelCheckOnAFrameSeesTheLevelsTheImbalanceCounts) {
    Loop loop;
    {
        Agent plain {Config()};
        ASSERT_TRUE(plain.imbalance);  // hold_ms 300, the opening order only
        EXPECT_EQ(plain.imbalance->levels, 5);
        EXPECT_EQ(plain.imbalance->ratio, 5.0);
        EXPECT_EQ(plain.imbalance->factor, 5.0);
        EXPECT_DOUBLE_EQ(plain.imbalance->hold_s, 0.3);
        EXPECT_EQ(plain.imbalance->sides, "open");
    }
    Config config;
    config.pricing.window = config.pricing.min_samples = 1;
    config.imbalance.hold_ms = 0;  // with a hold, the side found thin by evaluate would hide a check that sees less
    config.imbalance.sides = "both";  // the sell below takes the thin-side markup too
    Agent agent(config);
    StrategyOptions o = opts(0.0001, 0.00001);
    o.imbalance = agent.imbalance;
    FakeGateway* gw = install(agent, o);
    gw->add_pos(Decimal("0.001"));  // long: it sells, and the asks are the thin side below
    EXPECT_EQ(agent.imbalance->hold_s, 0.0);
    EXPECT_EQ(agent.imbalance->sides, "both");
    agent.reference_book.apply_snapshot(lv("100000"), lv("100000.1"), 1);
    // 1.2 BTC on each of the first 5 bid levels, 1 BTC of asks: 6x over 5 levels, 2.4x over the top 2
    JsonArray bids;
    for (int i = 0; i < 5; ++i) bids.push_back(JsonArray {std::format("{:.1f}", 99'990 - 0.1 * i).c_str(), "1.2"});
    agent.venue_book.apply_snapshot(bids, lv("99990.1"), 1);
    frame(agent, agent.venue_book);
    evaluate(agent);
    gw->ack();
    ASSERT_EQ(gw->sent.size(), 1u);
    EXPECT_EQ(gw->sent[0].second->side, "SELL");
    EXPECT_EQ(gw->sent[0].second->price, Decimal("100040.1"));  // 5x markup: asks are thin
    frame(agent, agent.reference_book);  // a frame: the check must price it the same way
    EXPECT_EQ(gw->kinds(), (Kinds {"place"}));
}

TEST(Agent, DirectionIsReadAndChecked) {
    TempFile f;
    f.write("[trading]\ndirection = \"short\"\n");
    EXPECT_EQ(load_config(f.written).trading.direction, "short");
    f.write("[trading]\ndirection = \"both\"\n");
    auto both = load_config(f.written).trading;
    EXPECT_EQ(both.direction, "both");
    EXPECT_EQ(both.max_position_usd, 1000.0);  // $1000 either way by default
    f.write("[trading]\ndirection = \"both\"\nmax_position_usd = 2500\n");
    EXPECT_EQ(load_config(f.written).trading.max_position_usd, 2500.0);
    f.write("[trading]\ndirection = \"sideways\"\n");
    EXPECT_LOAD_ERROR(f.written, {}, "direction");
    f.write("[trading]\nmax_position_usd = 0\n");
    EXPECT_LOAD_ERROR(f.written, {}, "max_position_usd");
}

TEST(Agent, CloseTolAwayIsReadAndChecked) {
    TempFile f;
    f.write("[trading]\nclose_tol_away = 0.000005\n");
    EXPECT_EQ(load_config(f.written).trading.close_tol_away, 0.000005);
    f.write("[trading]\nclose_tol_away = -0.1\n");
    EXPECT_LOAD_ERROR(f.written, {}, "close_tol_away");
}

TEST(Agent, ImbalanceSettingsAreChecked) {
    TempFile f;
    f.write("[imbalance]\nratio = 0\n");
    EXPECT_EQ(load_config(f.written).imbalance.ratio, 0);  // off
    EXPECT_EQ(load_config(f.written).imbalance.sides, "open");  // the closing order keeps its markup by default
    f.write("[imbalance]\nsides = \"both\"\n");
    EXPECT_EQ(load_config(f.written).imbalance.sides, "both");
    f.write("[imbalance]\nratio = 50\nsides = \"buy\"\n");  // the old name of "open"
    EXPECT_EQ(Agent(load_config(f.written)).imbalance->sides, "open");
    for (const char* bad : {"levels = 11", "ratio = 1", "factor = 0.5", "sides = \"sell\"", "sides = \"BUY\""}) {
        f.write(std::format("[imbalance]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "imbalance");
    }
}

TEST(Agent, ArcusRallyPullsASellAndASlidePullsABuy) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    {
        Bot b = guarded_agent("SELL");  // the Arcus guard looks at the last 10 ms
        Agent& agent = *b.agent;
        agent.venue_book.apply_snapshot(lv("99990"), lv("99990.1"), 2);
        frame(agent, agent.venue_book);
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));
        agent.venue_book.apply_snapshot(lv("100010"), lv("100010.1"), 3);  // the Arcus mid 2 bp over its mean: a rally
        frame(agent, agent.venue_book);
        EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
        EXPECT_TRUE(contains(log.messages(),
                             "!!! RISE Arcus mid 100010.05 is 2.00 bp over its mean 99990.05 over the last 10 ms: no selling for 50 ms"))
            << join(log.messages(), "\n");
        EXPECT_TRUE(agent.strategy->paused("SELL"));
        EXPECT_FALSE(agent.strategy->paused("BUY"));
    }
    Bot b = guarded_agent("BUY");
    Agent& agent = *b.agent;
    agent.venue_book.apply_snapshot(lv("99990"), lv("99990.1"), 2);
    frame(agent, agent.venue_book);
    agent.venue_book.apply_snapshot(lv("99970"), lv("99970.1"), 3);  // a slide
    frame(agent, agent.venue_book);
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", "BUY"}, {"cancel", "BUY"}}));
}

TEST(Agent, ArcusGuardSettingsAreChecked) {
    TempFile f;
    f.write("[arcus_guard]\ndrawdown = 0\n");
    EXPECT_EQ(Agent(load_config(f.written)).arcus_guard, nullptr);  // off
    for (const char* bad : {"window_ms = 0", "drawdown = 1", "pause_ms = -1"}) {
        f.write(std::format("[arcus_guard]\n{}\n", bad));
        EXPECT_LOAD_ERROR(f.written, {}, "arcus_guard");
    }
}

TEST(Agent, GuardWindowsAreTimeSpans) {
    TempFile f;
    f.write("[guard]\nwindow_ms = 2500\n[futures]\nwindow_ms = 500\n");
    Agent agent(load_config(f.written));
    EXPECT_EQ(agent.guard->window_s, 2.5);
    EXPECT_EQ(agent.futures_guard->window_s, 0.5);
    EXPECT_EQ(agent.arcus_guard->window_s, 3.0);  // the default
    for (std::string section : {"guard", "futures", "arcus_guard"}) {
        f.write(std::format("[{}]\nwindow = 100\n", section));  // a config from before: updates, not ms
        EXPECT_LOAD_ERROR(f.written, {}, std::format("{}.window is gone: use {}.window_ms", section, section));
    }
    f.write("[guard]\nwindow_ms = 0\n");
    EXPECT_LOAD_ERROR(f.written, {}, "guard needs 0 < window_ms");
}

TEST(Agent, FairPricedOffFuturesMovesWithFuturesNotSpot) {
    Loop loop;
    Bot b = futures_priced_agent();
    Agent& agent = *b.agent;
    EXPECT_EQ(agent.price_book, agent.futures_book.get());
    EXPECT_EQ(agent.futures_pricer, agent.pricer);
    frame(agent, *agent.futures_book);  // a futures update samples the Arcus bid too
    double basis = 99'990 / 99'950.1;
    ASSERT_TRUE(agent.fair_bid);
    EXPECT_NEAR(*agent.fair_bid, 99'950.1 * basis, 1e-6);  // = the Arcus bid, before anything moves
    frame(agent, agent.venue_book);
    EXPECT_NEAR(*agent.fair_bid, 99'990.0, 1e-6);
    b.clock.advance(0.05);
    A::changed(agent).clear();
    agent.reference_book.apply_snapshot(lv("99100"), lv("99100.1"), 2);  // spot jumps 10 bp
    frame(agent, agent.reference_book);
    EXPECT_NEAR(*agent.fair_bid, 99'950.1 * basis, 1e-6);  // spot does not move this fair
    // but the spot fair kept for the logs follows spot: its basis is still the Arcus bid / the old spot mid
    EXPECT_NE(agent.spot_pricer, agent.pricer);
    ASSERT_TRUE(agent.spot_fair_bid);
    EXPECT_NEAR(*agent.spot_fair_bid, 99'100.05 * 99'990 / 99'000.05, 1e-6);
    EXPECT_EQ(agent.build_view()->spot_fair_bid, agent.spot_fair_bid);
    A::changed(agent).clear();
    agent.futures_book->apply_snapshot(lv("99960"), lv("99960.2"), 2);  // futures up $10
    frame(agent, *agent.futures_book);
    EXPECT_TRUE(A::changed(agent).is_set());  // a futures move wakes the strategy
    EXPECT_NEAR(*agent.fair_bid, 99'960.1 * *agent.pricer->ratio(), 1e-6);
    EXPECT_GT(*agent.fair_bid, 99'990 + 9);  // the fair follows the futures at once; the basis follows slowly
    A::changed(agent).clear();
    agent.futures_book->apply_snapshot(lv("99960", "2"), lv("99960.2", "3"), 3);  // sizes only
    frame(agent, *agent.futures_book);
    EXPECT_FALSE(A::changed(agent).is_set());  // the mid stood: nothing to price
    agent.futures_book->invalidate();
    frame(agent, *agent.futures_book);
    EXPECT_FALSE(agent.fair_bid);  // no futures, no price: the quote is pulled
}

TEST(Agent, PricedOffSpotTheFuturesFairIsKeptForTheLogs) {
    Loop loop;
    Bot b = futures_priced_agent("spot", 0.0);
    Agent& agent = *b.agent;
    EXPECT_EQ(agent.price_book, &agent.reference_book);
    EXPECT_NE(agent.futures_pricer, agent.pricer);
    frame(agent, agent.venue_book);
    frame(agent, *agent.futures_book);
    EXPECT_NEAR(*agent.fair_bid, 99'000.0 * 99'990 / 99'000, 1e-6);  // spot bid x ratio, as before
    EXPECT_NEAR(*agent.futures_fair_bid, 99'990.0, 1e-6);  // futures mid x basis: the Arcus bid now
    auto view = agent.build_view();
    EXPECT_EQ(view->fair_bid, agent.fair_bid);
    EXPECT_EQ(view->futures_fair_bid, agent.futures_fair_bid);
}

TEST(Agent, PricingReferenceSettingsAreChecked) {
    TempFile f;
    f.write("[pricing]\nreference = \"futures\"\nbasis_s = 30\n");
    Config config = load_config(f.written);
    EXPECT_EQ(config.pricing.reference, "futures");
    EXPECT_EQ(config.pricing.basis_s, 30.0);
    f.write("[pricing]\nreference = \"perp\"\n");
    EXPECT_LOAD_ERROR(f.written, {}, "reference");
    f.write("[pricing]\nbasis_s = -1\n");
    EXPECT_LOAD_ERROR(f.written, {}, "basis_s");
    f.write("[pricing]\nreference = \"futures\"\n[futures]\nenabled = false\n");
    EXPECT_LOAD_ERROR(f.written, {}, "needs [futures] enabled");
}

TEST(Agent, EveryFillLogsItsContextAndARoundTripItsResult) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = guarded_agent("BUY");
    Agent& agent = *b.agent;
    FakeGateway* gw = b.gw;
    gw->on_fill = [&agent](const OrderPtr& o, const Decimal& size) { A::on_fill(agent, o, size); };  // as start_trading wires it
    OrderPtr buy = gw->order();
    b.clock.advance(0.004);
    gw->fill_order("0.0004");
    std::vector<std::string> context;
    for (auto& m : log.messages())
        if (m.rfind("--- fill context", 0) == 0) context.push_back(m);
    ASSERT_EQ(context.size(), 1u);
    double edge = ((*agent.fair_bid + 0.05) / buy->price.to_double() - 1) * 1e4;  // fair mid over the buy price
    std::string head = std::format("--- fill context BUY 0.0004 @ 99890: fair bid {:.2f} (edge {:+.2f} bp); "
                                   "arcus 99990 x 1 / 99990.1 x 1; vs guard means binance bid +0.00, arcus mid +0.00 bp; order stood ",
                                   *agent.fair_bid, edge);
    EXPECT_EQ(context[0].substr(0, head.size()), head) << context[0];
    EXPECT_TRUE(std::regex_match(context[0].substr(head.size()), std::regex(R"(\d+ ms)"))) << context[0];
    EXPECT_TRUE(gw->round_trips->open());
    EXPECT_EQ(gw->round_trips->position(), Decimal("0.0004"));
    agent.venue_book.apply_snapshot(lv("99880"), lv("99880.1"), 2);  // the long is worth 99880 now
    frame(agent, agent.venue_book);
    double worst = (99'880 / buy->price.to_double() - 1) * 1e4;  // 10 under the buy: -1 bp
    EXPECT_NEAR(gw->round_trips->trip->worst_bps, worst, 1e-9);
    gw->confirm_cancel();  // the rest of the buy is pulled; the strategy sells the position
    OrderPtr sell = gw->place("SELL", buy->price + Decimal(1), Decimal("0.0004"));
    gw->fill_order("0.0004");
    EXPECT_EQ(sell->state, OrderState::FILLED);
    EXPECT_FALSE(gw->round_trips->open());
    std::vector<std::string> trip;
    for (auto& m : log.messages())
        if (m.rfind("=== ROUND TRIP", 0) == 0) trip.push_back(m);
    double gain = 1 / buy->price.to_double() * 1e4;
    ASSERT_EQ(trip.size(), 1u);
    EXPECT_EQ(trip[0], std::format("=== ROUND TRIP #1 long 0.0004: bought @ {}, sold @ {}, held 0.0 s, {:+.2f} bp ($+0.0004); "
                                   "worst {:+.2f} bp at 0.0 s; 2 fills; exit: 1 sell; all 1 trips $+0.0004 ({:+.2f} bp)",
                                   buy->price.fixed(2), sell->price.fixed(2), gain, worst, gain));
}

TEST(Agent, PricedOffFuturesTheSpotFairShowsOnPlacementsAndFills) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = futures_priced_agent();
    Agent& agent = *b.agent;
    FakeGateway* gw = install(agent, opts(0.0001, 0.01));
    gw->on_fill = [&agent](const OrderPtr& o, const Decimal& size) { A::on_fill(agent, o, size); };
    for (OrderBook* book : {agent.futures_book.get(), &agent.venue_book, &agent.reference_book}) frame(agent, *book);
    ASSERT_TRUE(agent.spot_fair_bid && agent.fair_bid);
    agent.reference_book.apply_snapshot(lv("99010"), lv("99010.1"), 2);  // spot 1 bp up: the two differ
    frame(agent, agent.reference_book);
    evaluate(agent);
    std::vector<std::string> placed;
    for (auto& m : log.messages())
        if (m.rfind(">>> PLACE", 0) == 0) placed.push_back(m);
    ASSERT_EQ(placed.size(), 1u);
    EXPECT_NE(placed[0].find(std::format(", spot fair bid {:.2f}; queue: ", *agent.spot_fair_bid)), std::string::npos) << placed[0];
    gw->ack();
    gw->fill_order("0.001");
    std::vector<std::string> context;
    for (auto& m : log.messages())
        if (m.rfind("--- fill context", 0) == 0) context.push_back(m);
    ASSERT_EQ(context.size(), 1u);
    EXPECT_TRUE(std::regex_search(context[0], std::regex(R"(^--- fill context BUY 0\.001 @ [\d.]+: fair bid [\d.]+ \(edge [+-][\d.]+ bp\); )"
                                                         R"(spot fair bid [\d.]+ \(edge [+-][\d.]+ bp\); futures [+-][\d.]+ bp vs spot for it; arcus )")))
        << context[0];
}

TEST(Agent, EverySymbolMustNameTheMarketAsset) {
    TempFile f;
    std::string eth = "[arcus]\nmarket = \"ETH-USD\"\n[binance]\nsymbol = \"ETHUSDT\"\n[futures]\nsymbol = \"ETHUSDT\"\n";
    f.write(eth + "[bybit]\nsymbol = \"ETHUSDT\"\n");
    EXPECT_EQ(load_config(f.written).futures.symbol, "ETHUSDT");
    f.write(eth);  // bybit left at its default, BTCUSDT
    EXPECT_LOAD_ERROR(f.written, {}, "bybit.symbol = 'BTCUSDT' is not ETH against the dollar");
    f.write(eth + "[bybit]\nenabled = false\n");  // a feed that is off does not matter
    EXPECT_FALSE(load_config(f.written).bybit.enabled);
    // Review: a prefix match took other markets for ETH
    for (std::string other : {"ETHBTC", "ETHFIUSDT"}) {
        std::string text = eth;
        text.replace(text.find("[binance]\nsymbol = \"ETHUSDT\""), std::string("[binance]\nsymbol = \"ETHUSDT\"").size(),
                     "[binance]\nsymbol = \"" + other + "\"");
        f.write(text + "[bybit]\nenabled = false\n");
        EXPECT_LOAD_ERROR(f.written, {}, "binance.symbol = '" + other + "' is not ETH");
    }
    f.write("[arcus]\nmarket = \"PEPE-USD\"\n[binance]\nsymbol = \"PEPEUSDT\"\n[futures]\nsymbol = \"1000PEPEUSDT\"\n[bybit]\nenabled = false\n");
    EXPECT_EQ(load_config(f.written).futures.symbol, "1000PEPEUSDT");
}

TEST(Agent, AMarketPricedOffAnotherAssetNamesIt) {
    // GLD-USD, a share of a gold ETF, is priced off gold: XAU, XAUT and PAXG, each only once listed.
    TempFile f;
    auto gld = [](const std::string& assets) {
        return "[arcus]\nmarket = \"GLD-USD\"\n" + assets +
               "[binance]\nsymbol = \"XAUTUSDT\"\n[futures]\nsymbol = \"XAUUSDT\"\n[bybit]\nsymbol = \"PAXGUSDT\"\n";
    };
    f.write(gld(""));
    EXPECT_LOAD_ERROR(f.written, {}, "binance.symbol = 'XAUTUSDT' is not GLD against the dollar");
    f.write(gld("feed_assets = [\"XAU\", \"XAUT\", \"paxg\"]\n"));
    EXPECT_EQ(strings(load_config(f.written).arcus.feed_assets), (std::vector<std::string> {"XAU", "XAUT", "paxg"}));
    f.write(gld("feed_assets = [\"XAU\", \"PAXG\"]\n"));  // XAUT is another asset than XAU
    EXPECT_LOAD_ERROR(f.written, {}, "binance.symbol = 'XAUTUSDT' is not GLD or XAU or PAXG against the dollar");
    for (const char* bad : {"[\"XAU/USD\"]", "[\"\"]", "[1]"}) {
        f.write(gld(std::format("feed_assets = {}\n", bad)));
        std::string err = load_error(f.written);
        EXPECT_TRUE(std::regex_search(err, std::regex("arcus.feed_assets: .* is not an asset name"))) << err;
    }
    f.write(gld("feed_assets = \"XAU\"\n"));
    EXPECT_LOAD_ERROR(f.written, {}, "arcus.feed_assets must be list");
    // the shipped GLD config passes the check
    EXPECT_FALSE(load_config(repo("config.gld.toml")).arcus.feed_assets.empty());
}

TEST(Agent, TheStatusLineGivesTheRatioOfPricesOfDifferentAssets) {
    // agent._basis: the ratio of the prices when they are of different assets
    Loop loop;
    LogCapture log("naive_mm.agent");
    Agent agent {Config()};
    agent.reference_book.apply_snapshot(lv("4000"), lv("4000.2"), 1);  // gold
    agent.venue_book.apply_snapshot(lv("366.8"), lv("366.82"), 1);  // a GLD share
    agent.log_status();
    EXPECT_TRUE(contains(log.messages(), std::format("basis arcus/binance mid: ratio {:.6f}", 366.81 / 4000.1))) << join(log.messages(), "\n");
    agent.venue_book.apply_snapshot(lv("4000.6"), lv("4000.8"), 2);
    agent.log_status();
    EXPECT_TRUE(contains(log.messages(), "basis arcus/binance mid: +1.50 bp")) << join(log.messages(), "\n");
}

TEST(Agent, ValuesMeantInBpAndNonFiniteNumbersAreRefused) {
    // Review: tol = 0.5 meant as 0.5 bp passed and the buy was never repriced toward the market;
    // nan and inf passed too.
    TempFile f;
    for (auto [text, match] : std::initializer_list<std::pair<const char*, const char*>> {
             {"[trading]\ntol = 0.5\n", "trading.tol = 0.5 is 5000 bp"},
             {"[guard]\ndrawdown = 0.5\n", "guard.drawdown = 0.5 is 5000 bp"},
             {"[trading]\nclose_tol_away = 1.0\n", "trading.close_tol_away"},
             {"[trading]\ntol = nan\n", "finite"},
             {"[trading]\nmarkup = inf\n", "finite"},
             {"[binance]\nmax_book_age_s = -1.0\n", "binance.max_book_age_s must be >= 0"},
             {"[agent]\nstatus_interval_s = 0.0\n", "status_interval_s"},
             {"[arcus]\nn_levels = 10\n", "n_levels must be at least twice agent.depth"}}) {
        f.write(text);
        EXPECT_LOAD_ERROR(f.written, {}, match);
    }
    f.write("[guard]\ndrawdown = 0\nwindow_ms = 0\n");  // a guard that is off: its window does not matter
    EXPECT_EQ(load_config(f.written).guard.drawdown, 0);
}

TEST(Agent, PricedOffFuturesAStaleFuturesTopIsNoPriceOnAnyFrame) {
    Loop loop;
    Bot b = futures_priced_agent();
    Agent& agent = *b.agent;
    for (OrderBook* book : {agent.futures_book.get(), &agent.venue_book, &agent.reference_book}) frame(agent, *book);
    EXPECT_TRUE(agent.fair_bid);
    agent.futures_book->updated_mono -= 3.0;  // no futures ticker for 3 s, links still up
    frame(agent, agent.reference_book);  // a spot frame
    EXPECT_FALSE(agent.fair_bid);
    EXPECT_FALSE(agent.build_view()->fair_bid);
    frame(agent, agent.venue_book);  // an Arcus frame: the same
    EXPECT_FALSE(agent.fair_bid);
    A::changed(agent).clear();
    agent.futures_book->apply_snapshot(lv("99950", "2"), lv("99950.2"), 5);  // back, at the mid it left at
    frame(agent, *agent.futures_book);
    EXPECT_TRUE(agent.fair_bid);
    EXPECT_TRUE(A::changed(agent).is_set());  // priced again at once
}

TEST(Agent, AFuturesStallSeenOnlyBySpotFramesEndsWithTheFirstFreshTicker) {
    Loop loop;
    Bot b = futures_priced_agent();
    Agent& agent = *b.agent;
    for (OrderBook* book : {agent.futures_book.get(), &agent.venue_book, &agent.reference_book}) frame(agent, *book);
    agent.futures_book->updated_mono -= 3.0;
    frame(agent, agent.reference_book);  // only spot frames during the stall
    EXPECT_FALSE(agent.fair_bid);
    A::changed(agent).clear();
    agent.futures_book->apply_snapshot(lv("99950", "3"), lv("99950.2"), 6);  // back at the same mid
    frame(agent, *agent.futures_book);
    EXPECT_TRUE(agent.fair_bid);
    EXPECT_TRUE(A::changed(agent).is_set());
}

TEST(Agent, TheClockOffsetAndTheRequestPoolsAreLogged) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    FakeServer server(loop.ioc);
    server.on_get("/v1/time", [](const HttpRequest&) {
        return HttpReply(200, std::format(R"({{"timeNs": {}}})", time_ns() - 3'000'000));  // Arcus 3 ms behind our clock
    });
    std::string account;
    server.on_get("/v1/rateLimit", [&account](const HttpRequest& r) {
        account = r.query.count("accountIndex") ? r.query.at("accountIndex") : "";
        return HttpReply(200, R"({"accountIndex": 1, "order": {"used": 1200, "cap": 21200, "nextAvailableMs": 0},
                                  "cancel": {"used": 0, "cap": 41200, "nextAvailableMs": 0}})");
    });
    server.start();
    Config config;
    config.trading.mode = "live";
    config.arcus.rest_url = "http://" + server.base();
    auto creds = ArcusCredentials::make("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", 1,
                                        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    Agent agent(config, false, creds);
    agent.gateway = std::make_unique<FakeGateway>();
    Task task = Task::spawn(A::exchange_watch(agent), "exchange-watch");
    EXPECT_TRUE(run_until(loop.ioc, [&] { return any_starts(log.messages(), "--- request pools"); }));
    task.cancel();
    loop.run_for(0.02);
    server.close();
    EXPECT_EQ(account, "1");
    auto messages = log.messages();
    auto clock = std::find_if(messages.begin(), messages.end(), [](const std::string& m) { return m.rfind("--- clock", 0) == 0; });
    ASSERT_NE(clock, messages.end());
    EXPECT_TRUE(std::regex_match(*clock, std::regex(R"(--- clock: host \+[\d.]+ ms against Arcus \(round trip [\d.]+ ms, the quickest of 5\))")))
        << *clock;
    ASSERT_TRUE(agent.gateway->clock_offset_ms);
    EXPECT_GT(*agent.gateway->clock_offset_ms, 2.0);
    EXPECT_LT(*agent.gateway->clock_offset_ms, 4.0);
    EXPECT_TRUE(contains(messages, "--- request pools: orders used 1200 of 21200 (20000 left), cancels used 0 of 41200 (41200 left)"))
        << join(messages, "\n");
}

TEST(Agent, AnArcusAnswerWithoutItsFieldsIsLoggedNotACrash) {
    // A throttled or broken answer ({"message": "Too Many Requests"}) has none of the fields: a failed
    // look, logged, as Python's KeyError (was a null dereference: the process died, orders resting)
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    FakeServer server(loop.ioc);
    server.on_get("/v1/time", [](const HttpRequest&) { return HttpReply(200, R"({"message": "Too Many Requests"})"); });
    server.on_get("/v1/rateLimit", [](const HttpRequest&) { return HttpReply(200, R"({"message": "Too Many Requests"})"); });
    server.start();
    Config config;
    config.trading.mode = "live";
    config.arcus.rest_url = "http://" + server.base();
    auto creds = ArcusCredentials::make("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", 1,
                                        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    Agent agent(config, false, creds);
    agent.gateway = std::make_unique<FakeGateway>();
    Task task = Task::spawn(A::exchange_watch(agent), "exchange-watch");
    EXPECT_TRUE(run_until(loop.ioc, [&] { return any_starts(log.messages(), "--- request pools"); }));
    task.cancel();
    loop.run_for(0.02);
    server.close();
    auto messages = log.messages();
    EXPECT_TRUE(contains(messages, "--- clock: GET /v1/time failed: no field 'timeNs'")) << join(messages, "\n");
    EXPECT_TRUE(contains(messages, "--- request pools: GET /v1/rateLimit failed: no field 'order'")) << join(messages, "\n");
    EXPECT_FALSE(agent.gateway->clock_offset_ms);
}

TEST(Agent, TheClosingMarkupCutIsChecked) {
    TempFile f;
    f.write("[trading]\nclose_markup_cut = 0.3\n");
    EXPECT_EQ(load_config(f.written).trading.close_markup_cut, 0.3);
    f.write("[trading]\nclose_markup_cut = 1.5\n");
    EXPECT_LOAD_ERROR(f.written, {}, "close_markup_cut");
}

TEST(Agent, APauseThatEndedBeforeItsTimerRanStillGetsItsLine) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = guarded_agent("BUY");
    Agent& agent = *b.agent;
    top(agent, agent.reference_book, "99985", 5);  // a drop: buying paused 50 ms
    b.clock.advance(0.06);  // over by the guard's clock, but the timer has not run yet
    top(agent, agent.reference_book, "100000", 6);
    b.clock.advance(0.011);  // a full window back at 100000
    top(agent, agent.reference_book, "99985", 7);  // a fresh drop before that timer ran
    std::vector<std::string> over, lines;
    for (auto& m : log.messages()) {
        if (m.find("pause over") != std::string::npos) over.push_back(m);
        if (m.find("pause over") != std::string::npos || m.rfind("!!! DROP", 0) == 0) lines.push_back(m);
    }
    EXPECT_EQ(over, (std::vector<std::string> {"--- Binance buying pause over: 50 ms, extended 0 times"}));
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[lines.size() - 2].rfind("--- Binance buying pause over", 0), 0u);  // in order
    EXPECT_EQ(lines.back().rfind("!!! DROP", 0), 0u);
}

TEST(Agent, TheGapHistoryIsDroppedWhileAFairIsMissing) {
    Loop loop;
    Bot b = futures_priced_agent();
    Agent& agent = *b.agent;
    agent.futures_fair_bid = 100'010.0;
    agent.spot_fair_bid = 100'000.0;
    A::note_gap(agent);
    EXPECT_EQ(A::gaps(agent).size(), 1u);
    EXPECT_NEAR(*A::gap_now(agent), 1.0, 1e-9);
    agent.futures_fair_bid.reset();  // futures out: no gap, and no change across the outage later
    A::note_gap(agent);
    EXPECT_TRUE(A::gaps(agent).empty());
    EXPECT_FALSE(A::gap_change(agent, A::gap_now(agent)));
}

TEST(Agent, TheArcusGuardKeepsItsWindowWhileTheBookIsRebuilt) {
    // Review: the Arcus book is rebuilt by design when a sweep took most of its known levels. The
    // guard was reset then and had no mean for a whole window: the first price after the rebuild,
    // far under where the market stood, tripped nothing, and the buy went straight back in.
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    Bot b = guarded_agent("BUY");
    Agent& agent = *b.agent;
    agent.venue_book.invalidate();  // rebuilding after the sweep: no price, the buy is pulled
    frame(agent, agent.venue_book);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));
    b.gw->confirm_cancel();
    b.clock.advance(0.005);  // half a window later it is back, 5 bp lower
    agent.venue_book.apply_snapshot(lv("99940"), lv("99940.1"), 5);
    frame(agent, agent.venue_book);
    EXPECT_TRUE(any_starts(log.messages(), "!!! DROP Arcus mid 99940.05")) << join(log.messages(), "\n");
    evaluate(agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));  // no buy during the pause
}

TEST(Agent, AfterALongGapTheArcusGuardStartsOver) {
    Loop loop;
    Bot b = guarded_agent("BUY");
    Agent& agent = *b.agent;
    agent.venue_book.invalidate();
    frame(agent, agent.venue_book);
    b.gw->confirm_cancel();
    b.clock.advance(0.05);  // five windows: the mean from before says nothing about now
    agent.venue_book.apply_snapshot(lv("99940"), lv("99940.1"), 5);
    frame(agent, agent.venue_book);
    evaluate(agent);
    EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel", "place"}));
}

TEST(Agent, PricedOffFuturesASpotFrameRunsTheCancelCheckOnlyWhenItFindsTheFuturesTopStale) {
    Loop loop;
    Bot b = futures_priced_agent();
    Agent& agent = *b.agent;
    FakeGateway* gw = install(agent, opts(0.001, 0.0002));
    for (OrderBook* book : {agent.futures_book.get(), &agent.venue_book, &agent.reference_book}) frame(agent, *book);
    evaluate(agent);
    gw->ack();
    gw->checks = 0;  // each cancel check asks the gateway whether a cancel can go out, first thing
    A::changed(agent).clear();
    agent.reference_book.apply_update(lv("99100"), JsonArray {}, 2);  // spot moves: nothing the cancel rules read
    frame(agent, agent.reference_book);
    EXPECT_EQ(gw->checks, 0);
    EXPECT_TRUE(A::changed(agent).is_set());  // the strategy loop still looks
    frame(agent, agent.venue_book);  // an Arcus frame: checked
    EXPECT_EQ(gw->checks, 1);
    agent.futures_book->updated_mono -= 3.0;  // no futures ticker for 3 s
    frame(agent, agent.reference_book);  // the spot frame that finds it: no price, the order goes
    EXPECT_EQ(gw->checks, 2);
    EXPECT_EQ(gw->kinds(), (Kinds {"place", "cancel"}));
    EXPECT_EQ(gw->why, "no price to quote at");
}
