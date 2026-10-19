// Helpers of the agent, config and command line tests: a gateway the test plays the
// exchange for, the agents the Python tests build, and access to the agent's insides.
#pragma once

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent.hpp"
#include "config.hpp"
#include "test_util.hpp"
#include "trading/gateway.hpp"
#include "trading/market.hpp"
#include "trading/strategy.hpp"
#include "util/json.hpp"

namespace nmm {

// What the tests reach of the agent (Python's _-prefixed attributes).
struct AgentTestAccess {
    static Event& changed(Agent& a) { return a.changed_; }
    static auto& resume_timers(Agent& a) { return a.resume_timers_; }
    static auto& gaps(Agent& a) { return a.gaps_; }
    static void note_gap(Agent& a) { a.note_gap(); }
    static std::optional<double> gap_now(Agent& a) { return a.gap_now(); }
    static std::optional<double> gap_change(Agent& a, std::optional<double> gap) { return a.gap_change(gap); }
    static void on_fill(Agent& a, const OrderPtr& o, const Decimal& size) { a.on_fill(o, size); }
    static void on_arcus_take(Agent& a, const JsonArray& trades) { a.on_arcus_take(trades); }
    static void score_takers(Agent& a, OrderBook& book) { a.score_takers(book); }
    static void on_trades_reset(Agent& a) { a.on_trades_reset(); }
    static std::optional<double>& last_trade(Agent& a) { return a.last_trade_; }
    static std::string drift_report(Agent& a) { return a.drift_report(); }
    static std::optional<ml::Prediction> ml_prediction(Agent& a) { return a.ml_prediction(); }
    static std::string pricing_summary(Agent& a) { return a.pricing_summary(); }
    static std::unique_ptr<BinanceTradesFeed> make_trades_feed(Agent& a) { return a.make_trades_feed(); }
    static awaitable<void> exchange_watch(Agent& a) { return a.exchange_watch(); }
};

namespace test::agent {

using A = AgentTestAccess;

// BTC-USD as Arcus lists it (test_trading_basics.BTC).
inline MarketSpec btc_spec() {
    return MarketSpec::from_json(parse_json(R"({
        "marketId": 1, "marketDisplayName": "BTC-USD", "tickSize": "0.1", "stepSize": "0.00000001",
        "tickTiers": [{"upToPrice": "500000", "tick": "0.1"}, {"upToPrice": "1000000", "tick": "0.2"}, {"tick": "5"}],
        "minOrderSize": "0.0001", "minOrderNotional": "5", "maxOrderSize": "10000"})"));
}

// Records requests; the test plays the exchange (test_strategy.FakeGateway).
class FakeGateway : public Gateway {
public:
    FakeGateway() : Gateway(btc_spec()) { ready = true; }

    awaitable<void> run() override { co_return; }
    // Every cancel check starts by asking this: the tests count them.
    bool can_cancel() const override {
        ++checks;
        return ready;
    }
    mutable int checks = 0;

    // The orders channel shows the order resting, as ArcusGateway handles it.
    void ack() {
        OrderPtr o = order();
        o->seen_open = true;
        set_state(*o, OrderState::OPEN);
    }
    void fill_order(const std::string& size) {
        OrderPtr o = order();
        fill(o, o->filled + Decimal(size));
        if (o->remaining() == 0) finish(o, OrderState::FILLED);
    }
    void confirm_cancel() { finish(order(), OrderState::CANCELED); }
    void add_pos(const Decimal& delta) { add_position(delta, std::nullopt); }

    // (kind, side) of what went out
    std::vector<std::pair<std::string, std::string>> sides() const {
        std::vector<std::pair<std::string, std::string>> out;
        for (auto& [kind, o] : sent) out.emplace_back(kind, o->side);
        return out;
    }
    std::vector<std::string> kinds() const {
        std::vector<std::string> out;
        for (auto& [kind, o] : sent) out.push_back(kind);
        return out;
    }

    std::vector<std::pair<std::string, OrderPtr>> sent;
    std::string why;

protected:
    void send_place(const OrderPtr& o) override { sent.emplace_back("place", o); }
    void send_cancel(const OrderPtr& o, const std::string& reason = "") override {
        sent.emplace_back("cancel", o);
        why = reason;
    }
};

using Kinds = std::vector<std::string>;
using Sides = std::vector<std::pair<std::string, std::string>>;

// [[price, size], ...] of one level per side.
inline Json lv(const std::string& price, const std::string& size = "1") { return JsonArray {JsonArray {price.c_str(), size.c_str()}}; }

// The agent's fake gateway after `install`.
inline FakeGateway* install(Agent& agent, StrategyOptions options = {}) {
    auto gw = std::make_unique<FakeGateway>();
    FakeGateway* raw = gw.get();
    agent.gateway = std::move(gw);
    if (options.order_size == 0) options.order_size = 0.001;
    agent.strategy = std::make_unique<QuoteStrategy>(raw, btc_spec(), options);
    return raw;
}

inline StrategyOptions opts(double markup, double tol, std::vector<Pauses*> guards = {}, std::string direction = "long") {
    StrategyOptions o;
    o.markup = markup;
    o.tol = tol;
    o.order_size = 0.001;
    o.guards = std::move(guards);
    o.direction = std::move(direction);
    return o;
}

// A file of `text` in a fresh temporary directory, removed with it.
struct TempFile {
    std::filesystem::path dir, path;
    explicit TempFile(const std::string& name = "config.toml") {
        char tmpl[] = "/tmp/nmm-agent-test-XXXXXX";
        dir = mkdtemp(tmpl);
        path = dir / name;
    }
    ~TempFile() { std::filesystem::remove_all(dir); }
    const std::string& write(const std::string& text) {
        std::ofstream(path) << text;
        written = path.string();
        return written;
    }
    std::string written;
};

// The message of the std::invalid_argument load_config throws ("" when it loads).
inline std::string load_error(const std::optional<std::string>& path, const std::vector<std::string>& overrides = {}) {
    try {
        load_config(path, overrides);
    } catch (const std::invalid_argument& err) {
        return err.what();
    }
    return "";
}

inline std::string repo(const std::string& name) { return std::string(NMM_SOURCE_DIR) + "/" + name; }

inline bool contains(const std::vector<std::string>& lines, const std::string& line) {
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

inline bool any_starts(const std::vector<std::string>& lines, const std::string& prefix) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.rfind(prefix, 0) == 0; });
}

}  // namespace test
}  // namespace nmm

#define EXPECT_LOAD_ERROR(path_or_null, overrides, needle)                                              \
    do {                                                                                                \
        std::string _err = ::nmm::test::load_error(path_or_null, overrides);                            \
        EXPECT_NE(_err.find(needle), std::string::npos) << "error: '" << _err << "' wants: " << (needle); \
    } while (0)

namespace nmm::test::agent {

// An event loop made the loop for the test's life.
struct Loop {
    boost::asio::io_context ioc;
    Loop() { set_loop(ioc); }
    void run_for(double seconds) { test::run_for(ioc, seconds); }
    void settle() {
        for (int i = 0; i < 5; ++i) {
            ioc.restart();
            ioc.poll();
        }
    }
};

struct Bot {
    std::unique_ptr<Agent> agent;
    FakeGateway* gw = nullptr;
    FakeClock clock;
    Agent* operator->() const { return agent.get(); }
};

inline void frame(Agent& agent, OrderBook& book) { agent.on_book_update(book); }

// A book's top is now `bid` / `bid` + 0.1 (one level each), under the next sequence.
inline void top(Agent& agent, OrderBook& book, const std::string& bid, int64_t seq) {
    book.apply_snapshot(lv(bid), lv(std::format("{:.1f}", std::stod(bid) + 0.1)), seq);
    agent.on_book_update(book);
}

inline void evaluate(Agent& agent) {
    auto view = agent.build_view();
    agent.strategy->evaluate(*view);
}

// An agent with a working order on `order_side` and a full guard window at 100000 behind each
// price guard (test_agent._guarded_agent). The guards read the bot's clock, which only moves
// when a test moves it.
inline Bot guarded_agent(const std::string& order_side) {
    Config c;
    c.pricing.window = c.pricing.min_samples = 1;
    for (auto* g : {&c.guard.window_ms, &c.futures.window_ms, &c.bybit.window_ms, &c.arcus_guard.window_ms}) *g = 10;
    c.guard.drawdown = c.futures.drawdown = c.bybit.drawdown = c.arcus_guard.drawdown = 0.0001;
    c.guard.pause_ms = c.futures.pause_ms = c.bybit.pause_ms = c.arcus_guard.pause_ms = 50;
    Bot b;
    b.agent = std::make_unique<Agent>(c);
    for (Pauses* g : b->guards) g->clock = b.clock.fn();
    // tol 1%: the moves below are far inside it, so only the guard can pull the order
    b.gw = install(*b.agent, opts(0.001, 0.01, b->guards));
    b->reference_book.apply_snapshot(lv("100000"), lv("100000.1"), 1);
    b->venue_book.apply_snapshot(lv("99990"), lv("99990.1"), 1);
    frame(*b.agent, b->venue_book);
    b->reference_book.apply_update(lv("100000", "2"), JsonArray {}, 2);
    frame(*b.agent, b->reference_book);
    b.clock.advance(0.01);  // a whole window at 100000 behind both guards
    if (order_side == "SELL") b.gw->add_pos(Decimal("0.001"));  // long: the strategy sells
    evaluate(*b.agent);
    b.gw->ack();
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", order_side}}));
    return b;
}

// test_agent._futures_priced_agent: books set up, nothing priced yet; EWMA pricers on the returned clock.
inline Bot futures_priced_agent(const std::string& reference = "futures", double basis_s = 30.0) {
    Config c;
    c.pricing.reference = reference;
    c.pricing.basis_s = basis_s;
    c.pricing.min_samples = 1;
    Bot b;
    b.agent = std::make_unique<Agent>(c);
    for (Pricer* p : {b->pricer.get(), b->futures_pricer.get(), b->spot_pricer.get()}) {
        if (auto* e = dynamic_cast<EwmaPricer*>(p)) {
            e->clock = b.clock.fn();
            e->min_weight = 0.0;  // these look at what the basis prices from; the warm-up has its own test
        }
    }
    b->reference_book.apply_snapshot(lv("99000"), lv("99000.1"), 1);
    b->venue_book.apply_snapshot(lv("99990"), lv("99990.1"), 1);
    b->futures_book->apply_snapshot(lv("99950"), lv("99950.2"), 1);
    return b;
}

}  // namespace nmm::test

// Each test helper header keeps its names apart (several define a FakeGateway or a view()).
namespace nmm::test {
using namespace nmm::test::agent;
}  // namespace nmm::test
