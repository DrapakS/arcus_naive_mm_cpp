// Agent: runs the venue feeds and hands a coalesced market view to the strategy.
//
// The feeds call on_book_update synchronously on every book change. That sets an
// event, which coalesces bursts: the strategy loop wakes once per turn of the
// event loop with the latest state of every book, however many updates arrived.
// Cancelling cannot wait for that: while an order works, the cancel rules run
// right there, on every frame. The pricing (basis or ML), the guards, the round
// trip marks and the log lines of every frame live here too.
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "config.hpp"
#include "feeds/arcus.hpp"
#include "feeds/base.hpp"
#include "feeds/binance.hpp"
#include "feeds/binance_futures.hpp"
#include "feeds/binance_trades.hpp"
#include "feeds/bybit.hpp"
#include "feeds/okx.hpp"
#include "health.hpp"
#include "ml.hpp"
#include "orderbook.hpp"
#include "portfolio.hpp"
#include "pricing.hpp"
#include "trading/credentials.hpp"
#include "trading/gateway.hpp"
#include "trading/guard.hpp"
#include "trading/opens.hpp"
#include "trading/quotes.hpp"
#include "trading/session.hpp"
#include "trading/strategy.hpp"
#include "util/aio.hpp"
#include "view.hpp"

namespace nmm {

// The time-weighted basis prices only once the ratios it has seen cover this much of its average
// (a tenth of a time constant: 3 s with basis_s = 30), and takes no sample from an Arcus book that
// has not changed for BASIS_SAMPLE_MAX_AGE_S: a frozen book (a stalled stream that still answers
// pings) would otherwise drag the basis along with every Binance move.
constexpr double BASIS_MIN_WEIGHT = 0.1;
constexpr double BASIS_SAMPLE_MAX_AGE_S = 5.0;
constexpr double EXCHANGE_WATCH_S = 600.0;  // how often the clock offset and the request pools are looked up
constexpr double SESSION_WATCH_S = 60.0;  // a market with hours: how often Arcus is asked whether it is outside them
constexpr double GAP_WINDOW_S = 2.0;  // the logs give the futures-vs-spot fair gap now and its change over this long
constexpr double GAP_SAMPLE_S = 0.05;  // the gap is sampled at most this often
constexpr double TOP_BEFORE_FILL_S = 0.1;  // the fill context also gives the Arcus top this long before the fill
constexpr int CLOCK_SAMPLES = 5;  // GET /v1/time round trips per look; the quickest one counts
constexpr double ML_TINY_WEIGHT = 1e-3;  // unscaled ML weights all under this: most likely fit on scaled features (warned)
constexpr double ML_RAW_THRESHOLD = 0.01;  // unscaled LightGBM splits typically above this: most likely fit on scaled features

class Agent {
public:
    Agent(Config config, bool show_ladder = false, std::optional<ArcusCredentials> credentials = std::nullopt);
    ~Agent();

    // Everything until stop(); then the trading is wound down (orders cancelled) and it returns.
    // Throws when the start fails (the market spec cannot be read) or a task fails.
    awaitable<void> run();
    // Ask run() to wind down and return (a signal handler calls this).
    void stop();

    // ---------------------------------------------------------- the parts
    Config config;
    bool show_ladder;
    int64_t ml_checks_skipped = 0;
    Clock tob_clock = monotonic;  // [agent] tob_line_ms is timed by it (tests set it)  // per-frame cancel checks the model's inputs left as they were
    std::optional<ArcusCredentials> credentials;
    std::unique_ptr<Gateway> gateway;
    std::unique_ptr<QuoteStrategy> strategy;  // with a gateway only: mode quote prices without one (quote_strategy)
    std::unique_ptr<QuoteStrategy> quote_strategy;
    std::unique_ptr<QuoteWatch> quotes;  // prints the buy/sell prices; all mode "quote" does
    std::unique_ptr<SessionClock> session_clock;  // a market with hours and an off-hours markup
    OrderBook reference_book;
    OrderBook venue_book;
    std::unique_ptr<OrderBook> futures_book;
    std::unique_ptr<OrderBook> bybit_book;
    std::unique_ptr<OrderBook> okx_book;
    std::vector<std::unique_ptr<BookFeed>> feeds;
    std::unique_ptr<BinanceTradesFeed> trades_feed;
    MarketViewPtr view;
    std::unique_ptr<ml::FeaturePricer> ml;
    Clock ml_clock = monotonic;  // the model's time: when each frame came, and "now" for its windows
    std::shared_ptr<Pricer> pricer, futures_pricer, spot_pricer;
    std::optional<double> fair_bid, futures_fair_bid, spot_fair_bid;
    std::unique_ptr<PriceGuard> guard, futures_guard, bybit_guard, okx_guard, arcus_guard;
    std::unique_ptr<SweepGuard> trades_guard;
    std::unique_ptr<TrendGuard> trend_guard, add_guard;
    std::unique_ptr<OpenWindows> open_guard;
    std::unique_ptr<Portfolio> portfolio;  // [portfolio], live only: the asset class's lean across the bots of the host
    std::unique_ptr<DriftWatch> drift;
    std::unique_ptr<LagGuard> lag_guard;
    std::unique_ptr<TakeGuard> take_guard;
    std::vector<Pauses*> guards;
    std::unique_ptr<LoopWatch> loop_watch;
    std::optional<Imbalance> imbalance;
    OrderBook* price_book;  // what the fair is priced off: spot or futures
    OrderBook* trend_book;  // the futures book, spot with [futures] off

    // ------------------------------------------------- per frame (tests drive these)
    void on_book_update(OrderBook& book);
    void on_trade(double price, int64_t trade_us, int64_t received_us);
    void on_market_data(const MarketView& view);
    MarketViewPtr build_view();
    // What the cancel rules need: the fair bid and the Arcus top levels.
    MarketView top_view();
    std::string health();
    void log_status();

private:
    friend struct AgentTestAccess;  // the tests reach the per-frame handlers and their state

    // The Binance spot trades of the [binance] symbol, by default over the [binance] URL.
    std::unique_ptr<BinanceTradesFeed> make_trades_feed();
    awaitable<void> start_trading();
    awaitable<void> exchange_watch();
    awaitable<void> session_watch();
    awaitable<void> market_data_loop();
    awaitable<void> ml_tick_loop();
    awaitable<void> status_loop();
    awaitable<void> health_loop();
    StrategyOptions strategy_options();
    void on_ml_update(OrderBook& book);
    void mark_trip(OrderBook& book);
    std::optional<ml::Prediction> ml_prediction();
    std::string ml_missing();
    void watch_moves(Pauses& guard, OrderBook& book, const std::string& label, double max_age = 0.0, bool mid = false);
    void watch_drift(OrderBook& book);
    std::string drift_report();
    void watch_adds(OrderBook& book);
    void on_arcus_take(const JsonArray& trades);
    void score_takers(OrderBook& book);
    void on_trades_reset();
    void tripped(Pauses& guard, const Trip& trip, const std::string& label, const std::string& why, const std::string& seen = "");
    void resume(Pauses* guard, const std::string& side, const std::string& label);
    void read_late(int64_t received_us, int64_t exchange_us, const std::string& what, double limit = 0.0);
    void loop_late(double lag_ms);
    void late(double lag_ms, const std::string& why);
    void lag_resume();
    void note_gap();
    std::optional<double> gap_now() const;
    std::optional<double> gap_change(std::optional<double> gap) const;
    void note_top(OrderBook& book);
    void on_fill(const OrderPtr& order, const Decimal& size);
    std::vector<std::tuple<std::string, std::optional<double>, Pricer*>> shadow_fairs() const;
    void log_tob(OrderBook& book, const std::string& label);
    void update_futures_fair(bool sample);
    void sample(Pricer& pricer, double reference_price);
    void update_spot_fair(OrderBook& updated);
    std::optional<double> venue_bid();
    double own_position() const;
    std::optional<double> arcus_mid() const;
    void print_ladders(OrderBook& updated);
    std::string pricing_summary();
    void fail(std::exception_ptr error);

    Logger* log_;
    Logger* events_;
    std::optional<double> last_trade_;  // the newest Binance spot trade's price
    std::optional<double> futures_mid_;  // the futures mid last priced off
    std::map<const OrderBook*, std::string> tob_labels_;  // [agent] tob_lines: the label of each book
    std::map<const OrderBook*, double> tob_last_;  // ... and the price last written for it
    std::map<const OrderBook*, double> tob_at_;  // ... and when (monotonic): [agent] tob_line_ms
    std::map<std::string, std::pair<int64_t, double>> drift_mark_;  // (episodes, seconds) at the last report
    TimerHandle lag_timer_;
    std::optional<std::pair<double, double>> lag_mark_;  // (clock, seconds paused) at the last health line
    std::map<std::pair<const Pauses*, std::string>, TimerHandle> resume_timers_;
    std::map<const Pauses*, double> guard_gaps_;  // since when the book of each guard has not been ready
    int top_levels_;
    std::vector<std::pair<OrderBook*, double>> ml_books_;  // the books the ML model reads, with their max age
    std::map<const OrderBook*, std::string> ml_venues_;  // the model's name for each book's venue
    std::vector<std::string> ml_skipped_;  // skip_missing: the features the last prediction went without
    // [ml] reuse_ms: (ml_clock, prediction) of the last one worked out, reused when asked again within it
    std::optional<std::pair<double, std::optional<ml::Prediction>>> ml_cached_;
    double ml_reuse_s_ = 0.0;
    // What the last per-frame cancel check with the model read, and when (ml_clock)
    struct MlCheckKey {
        double prediction_at;
        int64_t arcus_updates, arcus_bbos;
        Decimal position;
        std::vector<std::tuple<std::string, OrderState, Decimal, Decimal>> orders;
        bool drift_buy, drift_sell;
        bool operator==(const MlCheckKey&) const = default;
    };
    std::optional<MlCheckKey> ml_check_key_;
    double ml_check_at_ = 0.0;
    void ml_check_cancel(OrderBook& book);
    Event changed_;
    Event stop_;
    std::exception_ptr failure_;
    std::deque<std::pair<double, double>> gaps_;  // (monotonic, futures-vs-spot fair gap in bp)
    std::deque<std::tuple<double, double, double, double, double>> tops_;  // (t, bid, size, ask, size)
};

// Side-by-side top-N ladders: asks above the line (best ask lowest), bids below.
std::string format_ladders(const BookTop& left, const BookTop& right, std::optional<std::pair<std::string, std::string>> titles = {});

}  // namespace nmm
