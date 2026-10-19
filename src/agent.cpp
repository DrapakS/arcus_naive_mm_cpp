#include "agent.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "lgbm.hpp"
#include "net/net.hpp"
#include "trading/market.hpp"
#include "trading/paper.hpp"
#include "util/log.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

// A price in the status lines: ten significant digits.
std::string px(double price) { return std::format("{:.10g}", price); }

// Python's round(x, n): correctly rounded, as printf rounds.
double py_round(double x, int places) { return std::strtod(std::format("{:.{}f}", x, places).c_str(), nullptr); }

// Arcus over Binance in bp; the ratio of the prices when they are of different assets
// (arcus.feed_assets: a GLD share is ~0.092 of an ounce of gold, -9100 bp says nothing).
std::string basis_text(std::optional<double> bps) {
    if (!bps) return "n/a";
    return std::abs(*bps) < 1000 ? std::format("{:+.2f} bp", *bps) : std::format("ratio {:.6f}", 1.0 + *bps / 1e4);
}

// A price in a guard's line: two decimals, more for a coin under $10,000 (six digits in all).
std::string guard_px(double price) {
    int digits = static_cast<int>(std::to_string(static_cast<int64_t>(std::abs(price))).size());
    return std::format("{:.{}f}", price, std::max(2, 6 - digits));
}

// A guard's window or pause: in ms under 10 s, in s from there.
std::string span(double seconds) { return seconds < 10 ? std::format("{:.0f} ms", seconds * 1000) : std::format("{:.0f} s", seconds); }

std::string book_state(const OrderBook& book) {
    if (!book.synced) return "SYNCING";
    return book.is_crossed() ? "CROSSED" : "OK";
}

// The pricer's ratio spread for the status line ("n/a" below two samples).
std::string std_text(const Pricer& pricer) {
    auto s = pricer.ratio_std_bps();
    return s ? std::format("{:.2f} bp", *s) : "n/a";
}

std::string status_line(const BookFeed& feed) {
    const OrderBook& book = *feed.book;
    auto bid = book.best_bid(), ask = book.best_ask();
    std::vector<std::string> parts {std::format("{:<8} {:<8} {:<8}", book.venue, book.symbol, book_state(book))};
    parts.push_back(bid ? std::format("bid {} x {:<10.6g}", px(bid->first), bid->second) : "bid -");
    parts.push_back(ask ? std::format("ask {} x {:<10.6g}", px(ask->first), ask->second) : "ask -");
    if (book.synced) {
        parts.push_back(std::format("age {:.0f}ms", book.age() * 1e3));
        if (book.exchange_ts_us && *book.exchange_ts_us)
            parts.push_back(std::format("lat {:.0f}ms", static_cast<double>(book.local_ts_us - *book.exchange_ts_us) / 1e3));
    }
    std::string counts = std::format("seq {} upd {}", book.seq, book.update_count);
    if (book.bbo_count) counts += std::format(" bbo {}", book.bbo_count);
    counts += std::format(" msgs {} resync {}", feed.message_count, feed.resync_count);
    if (feed.refresh_count) counts += std::format(" refresh {}", feed.refresh_count);
    parts.push_back(std::format("{} conn {}", counts, feed.connect_count));
    if (feed.links.size() > 1) {
        // which connection delivered updates first, as shares of all updates
        int64_t total = 0;
        for (const auto& link : feed.links) total += link.wins;
        if (!total) total = 1;
        std::vector<std::string> shares;
        for (const auto& link : feed.links) shares.push_back(std::to_string(link.wins * 100 / total));
        parts.push_back(std::format("links {}/{} first {}%", feed.links_up(), feed.links.size(), join(shares, "/")));
    }
    return join(parts, "  ");
}

}  // namespace

std::string format_ladders(const BookTop& left, const BookTop& right, std::optional<std::pair<std::string, std::string>> titles) {
    size_t n_asks = std::max(left.asks.size(), right.asks.size());
    size_t n_bids = std::max(left.bids.size(), right.bids.size());
    std::string blank(38, ' ');
    auto rows = [&](const BookTop& top) {
        std::vector<std::string> asks, bids;
        for (auto it = top.asks.rbegin(); it != top.asks.rend(); ++it)
            asks.push_back(std::format("{:>12} {:>12} {:<12.6g}", "", px(it->first), it->second));
        for (const auto& [p, s] : top.bids) bids.push_back(std::format("{:>12.6g} {:>12} {:<12}", s, px(p), ""));
        std::vector<std::string> out(n_asks - asks.size(), blank);
        out.insert(out.end(), asks.begin(), asks.end());
        out.push_back(std::string(38, '-'));
        out.insert(out.end(), bids.begin(), bids.end());
        out.insert(out.end(), n_bids - bids.size(), blank);
        return out;
    };
    if (!titles) titles = std::pair {left.venue + " " + left.symbol, right.venue + " " + right.symbol};
    std::vector<std::string> lines {std::format("{:^38} | {:^38}", titles->first, titles->second)};
    auto a = rows(left), b = rows(right);
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) lines.push_back(a[i] + " | " + b[i]);
    return join(lines, "\n");
}

Agent::Agent(Config config_, bool show_ladder_, std::optional<ArcusCredentials> credentials_)
    : config(std::move(config_)), show_ladder(show_ladder_), credentials(std::move(credentials_)),
      reference_book("binance", config.binance.symbol, config.binance.snapshot_limit),
      // No depth cap: the Arcus feed bounds the book to the price range its snapshot covers.
      venue_book("arcus", config.arcus.market), log_(&get_logger("naive_mm.agent")), events_(&get_logger(ORDERS_LOGGER)) {
    if (config.trading.mode == "live" && !credentials) throw std::invalid_argument("live trading needs Arcus credentials");
    view = std::make_shared<MarketView>();
    const PricingConfig& p = config.pricing;
    // pricing.model picks one of two pricings, and only that one is computed: "basis" (the pricers
    // below) or "ml" (the model of [ml]), never both.
    bool basis = p.model == "basis";
    if (!basis) {
        const MlConfig& m = config.ml;
        if (m.kind == "lightgbm") ml = std::make_unique<ml::LightGBMPricer>(lgbm::load_model(m.lightgbm_model), strings(m.features), ml_params(m));
        else ml = std::make_unique<ml::LinearPricer>(numbers(m.weights), ml_params(m));
    }
    auto ewma = [&](double tau) { return std::make_shared<EwmaPricer>(tau, p.min_samples, monotonic, BASIS_MIN_WEIGHT); };
    if (basis) {
        if (p.basis_s) pricer = ewma(p.basis_s);
        else pricer = std::make_shared<RatioPricer>(p.window, p.min_samples);
        // Futures move first: with the futures feed on, a fair priced off futures is kept for the
        // logs (the edge of every fill and placement) even when the quotes are priced off spot;
        // the same the other way round, so the two can be compared on the same fills.
        spot_pricer = p.reference == "spot" ? pricer : ewma(p.basis_s ? p.basis_s : 30.0);
    }
    const GuardConfig& g = config.guard;
    const FuturesConfig& f = config.futures;
    // Pull the order a sharp Binance move threatens and hold off that side; none = off.
    if (g.drawdown) guard = std::make_unique<PriceGuard>(g.window_ms / 1000, g.drawdown, g.pause_ms / 1000);
    if (f.enabled) futures_book = std::make_unique<OrderBook>("binance-futures", f.symbol);
    if (futures_book && basis) futures_pricer = p.reference == "futures" ? pricer : ewma(p.basis_s ? p.basis_s : 30.0);
    price_book = p.reference == "futures" && futures_book ? futures_book.get() : &reference_book;
    if (f.enabled && f.drawdown) futures_guard = std::make_unique<PriceGuard>(f.window_ms / 1000, f.drawdown, f.pause_ms / 1000);
    // Binance spot trades, the earliest sign of a sweep: a trade printed through the high (low) of
    // the last moments pulls a buy (sell) at once.
    const TradesConfig& tt = config.trades;
    if (tt.enabled && tt.drawdown) trades_guard = std::make_unique<SweepGuard>(tt.window_ms / 1000, tt.drawdown, tt.pause_ms / 1000);
    const BybitConfig& y = config.bybit;
    if (y.enabled) bybit_book = std::make_unique<OrderBook>("bybit-" + y.category, y.symbol);
    if (y.enabled && y.drawdown) bybit_guard = std::make_unique<PriceGuard>(y.window_ms / 1000, y.drawdown, y.pause_ms / 1000);
    // [agent] tob_lines: the label of each book whose best bid gets a line when its price changes
    if (config.agent.tob_lines) {
        for (auto [book, label] : {std::pair<const OrderBook*, const char*> {futures_book.get(), "BINANCE FUTURES"},
                                   {&reference_book, "BINANCE SPOT"}, {bybit_book.get(), "BYBIT"}, {&venue_book, "ARCUS"}})
            if (book != nullptr) tob_labels_[book] = label;
    }
    const ArcusGuardConfig& a = config.arcus_guard;
    if (a.drawdown) arcus_guard = std::make_unique<PriceGuard>(a.window_ms / 1000, a.drawdown, a.pause_ms / 1000);
    // Minutes rather than milliseconds, and the opening side only: no buy into a fall (a long), no
    // sell into a rally (a short); the closing order is never held back. Both sides quoted: either
    // can open, so a fall holds the buys and a rally the sells.
    const TrendConfig& tr = config.trend;
    std::string open_side = config.trading.direction == "long" ? "BUY" : config.trading.direction == "short" ? "SELL" : "both";
    if (tr.drawdown) trend_guard = std::make_unique<TrendGuard>(tr.window_s, tr.drawdown, tr.pause_s, open_side);
    trend_book = futures_book ? futures_book.get() : &reference_book;
    // Both sides: after a move against a side, no adding there past a share of the limit (the
    // opening of a position from flat still goes). Not a pausing guard: it only cuts the size.
    const AddGuardConfig& ad = config.add_guard;
    if (ad.drawdown) add_guard = std::make_unique<TrendGuard>(ad.window_s, ad.drawdown, ad.pause_s, "both");
    const OpenGuardConfig& og = config.open_guard;  // [open_guard]: no adding around the underlying's opens
    if (!og.opens.empty()) open_guard = std::make_unique<OpenWindows>(strings(og.opens), og.tz, og.before_s, og.after_s);
    // The futures mid's move over the last few seconds: a side it went against takes a wider markup
    // (drift.markup_factor; 1 = off), and every placement and fill logs it either way.
    const DriftConfig& dr = config.drift;
    drift = std::make_unique<DriftWatch>(dr.window_s, dr.threshold, dr.hold_s);
    drift_mark_ = {{"BUY", {0, 0.0}}, {"SELL", {0, 0.0}}};
    // Market data read late (or a late event loop): both orders pulled, nothing placed until it is on time.
    const LagConfig& lg = config.lag;
    if (lg.max_ms) lag_guard = std::make_unique<LagGuard>(lg.max_ms, lg.pause_ms / 1000);
    const TakesConfig& k = config.takes;  // Arcus taker waves: a take pauses the side it hit
    if (k.pause_ms)
        take_guard = std::make_unique<TakeGuard>(k.min_usd, k.pause_ms / 1000, monotonic, k.min_takes, k.min_move, k.score_ms / 1000);
    for (Pauses* gp : std::initializer_list<Pauses*> {guard.get(), futures_guard.get(), trades_guard.get(), bybit_guard.get(),
                                                      arcus_guard.get(), trend_guard.get(), lag_guard.get(), take_guard.get()})
        if (gp != nullptr) guards.push_back(gp);
    // the event loop's lag and the CPU share, for the health line; each lag is a reading of the lag guard
    std::function<void(double)> on_lag;
    if (lag_guard) on_lag = [this](double ms) { loop_late(ms); };
    loop_watch = std::make_unique<LoopWatch>(*events_, LOOP_TICK_S, STALL_MS, STALL_LOG_EVERY_S, on_lag);
    const ImbalanceConfig& i = config.imbalance;
    std::string sides = i.sides == "buy" ? "open" : i.sides;  // "buy": the old name, from when the bot only went long
    if (i.ratio) imbalance = Imbalance {i.levels, i.ratio, i.factor, i.hold_ms / 1000, sides};
    // levels the per-frame cancel check needs: two to see past our own order, and the imbalance's
    top_levels_ = imbalance ? std::max(2, i.levels) : 2;
    // The books the ML model reads, each with the age past which it is no price (as for the basis pricing).
    if (ml) {
        std::map<std::string, std::pair<OrderBook*, double>> books {
            {"arcus", {&venue_book, config.arcus.max_book_age_s}},
            {"binance", {&reference_book, config.binance.max_book_age_s}},
            {"binance_futures", {futures_book.get(), config.futures.max_book_age_s}},
        };  // no Bybit for the model (its feed stays for the guard and the logs)
        for (const auto& venue : ml->venues) {
            auto [book, max_age] = books.at(venue);
            if (book == nullptr) throw std::invalid_argument("the ML model reads " + venue + "_* features, and that feed is off");
            if (std::find(ml->needed_venues.begin(), ml->needed_venues.end(), venue) != ml->needed_venues.end())
                ml_books_.emplace_back(book, max_age);  // skip_missing: the others may go quiet, the model prices on
            ml_venues_[book] = venue;
        }
    }
}

Agent::~Agent() = default;

void Agent::stop() { stop_.set(); }

void Agent::fail(std::exception_ptr error) {
    if (!failure_) failure_ = error;
    stop_.set();
}

awaitable<void> Agent::run() {
    const Config& cfg = config;
    if (ml) {
        log_->warning("ML pricing: {}", ml->describe());
        events_->info("--- pricing: ML, {}", ml->describe());
        bool unscaled = !ml->scaled;
        if (auto* linear = dynamic_cast<ml::LinearPricer*>(ml.get()); unscaled && linear != nullptr) {
            double biggest = 0.0;
            for (const auto& [feature, weight] : linear->terms) biggest = std::max(biggest, std::abs(weight));
            if (!linear->terms.empty() && biggest < ML_TINY_WEIGHT)
                // Raw features are fractions of 1e-4 or so: weights this small predict ~0 on them.
                log_->warning("ML weights are all under {:g}: fit on scaled features? Then give [ml.mean] and [ml.scale] (StandardScaler "
                              "mean_ and scale_) with ml.scaled = true", ML_TINY_WEIGHT);
        }
        if (auto* forest = dynamic_cast<ml::LightGBMPricer*>(ml.get());
            unscaled && forest != nullptr && forest->forest->threshold_scale() > ML_RAW_THRESHOLD)
            // Raw features are fractions of 1e-4 or so: splits at values of ~1 send them all one way.
            log_->warning("the LightGBM model splits at values of {:.2g} typically, the raw features are ~1e-4: fit on scaled features? "
                          "Then give [ml.mean] and [ml.scale] (StandardScaler mean_ and scale_) with ml.scaled = true",
                          forest->forest->threshold_scale());
    }
    if (cfg.trading.mode != "off") co_await start_trading();
    auto options = [this](double stale_timeout, int connections) {
        FeedOptions o;
        o.on_update = [this](OrderBook& book) { on_book_update(book); };
        o.stale_timeout = stale_timeout;
        o.connections = connections;
        return o;
    };
    feeds.push_back(std::make_unique<BinanceSpotBookFeed>(reference_book, cfg.binance.symbol, cfg.binance.ws_url, cfg.binance.rest_url,
                                                          cfg.binance.snapshot_limit, cfg.binance.update_speed_ms, cfg.binance.book_ticker,
                                                          options(cfg.binance.stale_timeout_s, cfg.binance.connections)));
    feeds.push_back(std::make_unique<ArcusBookFeed>(venue_book, cfg.arcus.market, cfg.arcus.ws_url, cfg.arcus.rest_url, cfg.arcus.n_levels,
                                                    cfg.agent.depth, options(cfg.arcus.stale_timeout_s, cfg.arcus.connections)));
    if (futures_book)
        feeds.push_back(std::make_unique<BinanceFuturesTickerFeed>(*futures_book, cfg.futures.symbol, cfg.futures.ws_url,
                                                                   options(cfg.futures.stale_timeout_s, cfg.futures.connections)));
    if (cfg.trades.enabled)  // the Binance spot trades of the [binance] symbol, by default over the [binance] URL
        trades_feed = std::make_unique<BinanceTradesFeed>(
            cfg.binance.symbol, cfg.trades.ws_url.empty() ? cfg.binance.ws_url : cfg.trades.ws_url,
            [this](double price, int64_t trade_us, int64_t received_us) { on_trade(price, trade_us, received_us); },
            [this] { on_trades_reset(); }, options(cfg.trades.stale_timeout_s, cfg.trades.connections));
    if (bybit_book)
        feeds.push_back(std::make_unique<BybitTickerFeed>(*bybit_book, cfg.bybit.category, cfg.bybit.symbol, cfg.bybit.ws_url,
                                                          options(cfg.bybit.stale_timeout_s, cfg.bybit.connections)));
    std::vector<Task> tasks;
    auto on_failure = [this](std::exception_ptr e) { fail(e); };
    auto spawn = [&](awaitable<void> aw, std::string name) { tasks.push_back(Task::spawn(std::move(aw), std::move(name), "naive_mm.agent", on_failure)); };
    for (auto& feed : feeds) spawn(feed->run(), "feed-" + feed->name);
    if (trades_feed) spawn(trades_feed->run(), "feed-" + trades_feed->name);
    spawn(market_data_loop(), "market-data");
    if (ml && cfg.ml.eval_interval_ms) spawn(ml_tick_loop(), "ml-tick");
    spawn(status_loop(), "status");
    if (cfg.agent.health_interval_s || lag_guard) spawn(loop_watch->run(), "loop-watch");
    if (cfg.agent.health_interval_s) spawn(health_loop(), "health");
    Task trading;
    if (gateway) {
        trading = Task::spawn(gateway->run(), "trading", "naive_mm.agent", on_failure);
        spawn(exchange_watch(), "exchange-watch");
    }
    if (session_clock) spawn(session_watch(), "session-watch");

    co_await stop_.wait();

    // Wind down: market data and the strategy first, then the orders, and only then the trading connections.
    for (auto& t : tasks) t.cancel();
    if (gateway) {
        co_await gateway->shutdown();
        trading.cancel();
    }
    co_await sleep(0.05);  // the cancelled tasks finish their cleanup
    if (gateway) gateway->round_trips->report_final();  // what is still open, at the last Arcus top seen with it open
    if (failure_) std::rethrow_exception(failure_);
}

awaitable<void> Agent::exchange_watch() {
    // At start and every EXCHANGE_WATCH_S: the host clock against Arcus's (GET /v1/time, the
    // quickest of a few round trips), so the exchange's own times in the logs can be set against
    // ours, and, trading live, what is left of the subaccount's request pools (GET /v1/rateLimit).
    // Only logs: a failed look is logged and tried again next time.
    std::string rest(rstrip(config.arcus.rest_url, '/'));
    for (;;) {
        std::string failure;
        try {
            std::optional<std::pair<int64_t, double>> best;
            for (int i = 0; i < CLOCK_SAMPLES; ++i) {
                int64_t sent = time_ns();
                HttpResponse resp = co_await http_get(rest + "/v1/time", {}, 5.0);
                int64_t server = to_int(*jget(parse_json(resp.body), "timeNs"));
                int64_t rtt = time_ns() - sent;
                if (!best || rtt < best->first)
                    best = std::pair {rtt, (static_cast<double>(sent) + static_cast<double>(rtt) / 2 - static_cast<double>(server)) / 1e6};
            }
            auto [rtt, offset] = *best;
            gateway->clock_offset_ms = offset;
            events_->info("--- clock: host {:+.1f} ms against Arcus (round trip {:.1f} ms, the quickest of {})", offset,
                          static_cast<double>(rtt) / 1e6, CLOCK_SAMPLES);
        } catch (const boost::system::system_error& err) {
            if (err.code() == asio::error::operation_aborted) throw;
            failure = err.what();
        } catch (const std::exception& err) {
            failure = err.what();
        }
        if (!failure.empty()) events_->info("--- clock: GET /v1/time failed: {}", failure);
        if (credentials && config.trading.mode == "live") {
            std::string url = std::format("{}/v1/rateLimit?address={}&accountIndex={}", rest, credentials->address, credentials->account_index);
            failure.clear();
            try {
                HttpResponse resp = co_await http_get(url, {}, 5.0);
                Json body = parse_json(resp.body);
                std::vector<std::string> pools;
                for (const char* name : {"order", "cancel"}) {
                    const Json& pool = *jget(body, name);
                    int64_t used = to_int(*jget(pool, "used")), cap = to_int(*jget(pool, "cap"));
                    std::string next = truthy(jget(pool, "nextAvailableMs")) ? std::format(", next in {} ms", to_str(*jget(pool, "nextAvailableMs"))) : "";
                    pools.push_back(std::format("{}s used {} of {} ({} left{})", name, used, cap, cap - used, next));
                }
                events_->info("--- request pools: {}", join(pools, ", "));
            } catch (const boost::system::system_error& err) {
                if (err.code() == asio::error::operation_aborted) throw;
                failure = err.what();
            } catch (const std::exception& err) {
                failure = err.what();
            }
            if (!failure.empty()) events_->info("--- request pools: GET /v1/rateLimit failed: {}", failure);
        }
        co_await sleep(EXCHANGE_WATCH_S);
    }
}

StrategyOptions Agent::strategy_options() {
    const TradingConfig& t = config.trading;
    StrategyOptions o;
    o.markup = t.markup;
    o.tol = t.tol;
    o.order_size = t.order_size;
    o.guards = guards;
    o.never_lead = t.never_lead;
    o.imbalance = imbalance;
    o.tol_away = t.tol_away;
    o.close_markup_cut = t.close_markup_cut;
    o.close_tol_away = t.close_tol_away;
    o.direction = t.direction;
    o.max_position_usd = t.max_position_usd;
    o.max_position_resume = t.max_position_resume;
    o.session = session_clock.get();
    o.off_hours_markup_factor = t.off_hours_markup_factor;
    o.skew_open = t.skew_open;
    o.skew_close = t.skew_close;
    o.exit_after_s = t.exit_after_s;
    o.exit_loss = t.exit_loss;
    o.add_guard = add_guard.get();
    o.add_limit_share = config.add_guard.limit_share;
    o.drift = drift.get();
    o.drift_factor = config.drift.markup_factor;
    o.came_hold_s = t.came_hold_ms / 1000;
    o.open_guard = open_guard.get();
    o.open_limit_share = config.open_guard.limit_share;
    return o;
}

awaitable<void> Agent::session_watch() {
    // Every SESSION_WATCH_S: whether Arcus says the market is outside its hours now (isOutsideRth:
    // the holidays the clock does not know). A failed look keeps the last answer.
    for (;;) {
        co_await sleep(SESSION_WATCH_S);
        std::optional<MarketSpec> spec;
        std::string failure;
        try {
            spec = co_await fetch_market(config.arcus.rest_url, config.arcus.market);
        } catch (const boost::system::system_error& err) {
            if (err.code() == asio::error::operation_aborted) throw;
            failure = err.what();
        } catch (const std::exception& err) {
            failure = err.what();
        }
        if (!spec) {
            events_->info("--- trading session: GET /v1/markets failed: {}", failure);
            continue;
        }
        session_clock->report(spec->outside_rth);
    }
}

awaitable<void> Agent::start_trading() {
    const TradingConfig& t = config.trading;
    MarketSpec market = co_await fetch_market(config.arcus.rest_url, config.arcus.market);
    if (t.off_hours_markup_factor != 1) {
        if (!market.hours) {
            log_->warning("off_hours_markup_factor {:g}: {} has no trading hours, the markup is the same at all hours", t.off_hours_markup_factor,
                          market.symbol);
        } else {
            session_clock = std::make_unique<SessionClock>(*market.hours, market.outside_rth, wall_time,
                                                           std::format(", markup x{:g}", t.off_hours_markup_factor));
            log_->warning("{} trades {}: outside that the markup is x{:g}", market.symbol, market.hours->describe(), t.off_hours_markup_factor);
            session_clock->outside();  // says at once if it is closed now
        }
    }
    if (open_guard) {
        auto next = open_guard->next_open();
        log_->warning("market opens {}; the next: {}", open_guard->describe(),
                      next ? std::format("{} ({} UTC)", next->first.text, utc_strftime(next->second, "%a %d %b %H:%M")) : "none");
        open_guard->window();  // says at once if one holds now
    }
    if (t.mode == "quote") {
        quote_strategy = std::make_unique<QuoteStrategy>(nullptr, market, strategy_options());
        quotes = std::make_unique<QuoteWatch>(*quote_strategy, config.agent.quote_line_ms / 1000);
        log_->warning("quote mode on {}: no orders; buy and sell prices the bot would quote go to stdout (markup {}, tol {})", market.symbol,
                      pyrepr(t.markup), pyrepr(t.tol));
        co_return;
    }
    auto changed = [this] { changed_.set(); };
    if (t.mode == "paper") {
        gateway = std::make_unique<PaperGateway>(market, config.arcus.ws_url, changed);
    } else {
        ArcusGateway::Options o;
        o.ws_url = config.arcus.ws_url;
        o.dms_timeout = t.dms_timeout_s;
        o.cancel_lanes = t.cancel_lanes;
        o.presign_cancel = t.presign_cancel_s;
        o.rest_url = config.arcus.rest_url;
        o.slow_reconnect_ms = t.slow_reconnect_ms;
        gateway = std::make_unique<ArcusGateway>(market, *credentials, o, changed);
    }
    gateway->on_fill = [this](const OrderPtr& order, const Decimal& size) { on_fill(order, size); };
    if (take_guard) gateway->on_trades = [this](const JsonArray& trades) { on_arcus_take(trades); };
    strategy = std::make_unique<QuoteStrategy>(gateway.get(), market, strategy_options());
    // the prices it quotes, with the working order
    quotes = std::make_unique<QuoteWatch>(*strategy, config.agent.quote_line_ms / 1000);
    std::string how = t.direction == "both"
                          ? std::format("BOTH sides, {} each, the position within ${:g} either way", pyrepr(t.order_size), t.max_position_usd)
                          : std::format("{}, {} {} first", upper(t.direction), t.direction == "long" ? "buy" : "sell", pyrepr(t.order_size));
    log_->warning("trading {} on {}: {}, markup {}{}, tol {}{}; buy and sell prices go to stdout", upper(t.mode), market.symbol, how,
                  pyrepr(t.markup), t.close_markup_cut ? std::format(" (closing {} less)", percent(t.close_markup_cut)) : "", pyrepr(t.tol),
                  t.mode == "live" ? ", account " + credentials->str() : "");
}

// ---------------------------------------------------------------- market data

void Agent::on_book_update(OrderBook& book) {
    // Called synchronously by the feeds on every book change. Setting an event coalesces bursts:
    // the strategy wakes once per loop iteration with the latest state of both books, however many
    // updates arrived. Cancelling cannot wait for that: while an order works, the cancel rules run
    // right here, on every frame.
    if (auto it = tob_labels_.find(&book); it != tob_labels_.end()) log_tob(book, it->second);
    if (&book == &venue_book && take_guard && take_guard->scoring()) score_takers(book);
    if (&book == trend_book) {
        watch_drift(book);
        if (add_guard) watch_adds(book);
    }
    if (ml) {
        on_ml_update(book);
        return;
    }
    if (&book == futures_book.get()) {
        if (lag_guard && book.exchange_ts_us && *book.exchange_ts_us)
            read_late(book.local_ts_us, *book.exchange_ts_us, "Binance futures ticker");
        if (futures_guard) watch_moves(*futures_guard, book, "Binance futures");
        if (trend_guard) watch_moves(*trend_guard, book, "trend: Binance futures", 0.0, true);
        // Most tickers only change a size: the mid, all the pricing reads, stands.
        std::optional<double> mid = book.is_ready() ? book.mid() : std::nullopt;
        if (mid == futures_mid_) return;
        futures_mid_ = mid;
        update_futures_fair(futures_pricer && futures_pricer->timed());
        if (&book != price_book) return;  // priced off spot: futures only feed their guard and the logs
        fair_bid = futures_fair_bid;
        changed_.set();
        if (strategy && gateway && !gateway->orders.empty()) strategy->check_cancel(top_view());
        return;
    }
    if (&book == bybit_book.get()) {  // the same for Bybit
        if (lag_guard && book.exchange_ts_us && *book.exchange_ts_us && config.lag.bybit_max_ms)
            read_late(book.local_ts_us, *book.exchange_ts_us, "Bybit ticker", config.lag.bybit_max_ms);
        if (bybit_guard) watch_moves(*bybit_guard, book, "Bybit " + config.bybit.category);
        return;
    }
    if (&book == &venue_book && futures_pricer) update_futures_fair(true);
    bool shadow_spot = spot_pricer != pricer;
    auto fair_before = fair_bid;
    if (shadow_spot) {  // priced off futures: a futures top gone stale since its last frame is no price
        if (futures_fair_bid && !futures_book->is_ready(config.futures.max_book_age_s)) {
            futures_fair_bid.reset();
            futures_mid_.reset();  // so the first fresh ticker prices again, even at the old mid
        }
        fair_bid = futures_fair_bid;
    } else {
        update_spot_fair(book);
        fair_bid = spot_fair_bid;
    }
    changed_.set();
    if (&book == &reference_book && guard) watch_moves(*guard, book, "Binance", config.binance.max_book_age_s);
    if (&book == trend_book && trend_guard) {  // [futures] off: spot it is
        watch_moves(*trend_guard, book, "trend: Binance", config.binance.max_book_age_s, true);
    } else if (&book == &venue_book && arcus_guard) {
        watch_moves(*arcus_guard, book, "Arcus", config.arcus.max_book_age_s, true);
    }
    // Priced off futures, a spot frame changes nothing the cancel rules read (the fair bid, the
    // Arcus top) unless it found the futures top gone stale; the strategy loop, woken above, looks
    // again in any case.
    bool moved = &book != &reference_book || !shadow_spot || fair_bid != fair_before;
    if (moved && strategy && gateway && !gateway->orders.empty()) strategy->check_cancel(top_view());
    if (shadow_spot) update_spot_fair(book);  // for the logs only: after the cancel check
    if (gateway) {  // for the logs, after the cancel check: the gap, the Arcus top
        note_gap();
        if (&book == &venue_book) note_top(book);
    }
    if (&book == &venue_book) mark_trip(book);
    if (show_ladder) print_ladders(book);
}

void Agent::on_ml_update(OrderBook& book) {
    // A book frame with pricing.model = "ml": the guards and the cancel rules as with the basis
    // pricing, and the frame of every book the model reads goes to it. Nothing of the basis pricing
    // runs; the fair is the model's at the time it is read (ml_prediction), as its windows slide.
    const Config& cfg = config;
    if (&book == futures_book.get()) {
        if (lag_guard && book.exchange_ts_us && *book.exchange_ts_us)
            read_late(book.local_ts_us, *book.exchange_ts_us, "Binance futures ticker");
        if (futures_guard) watch_moves(*futures_guard, book, "Binance futures");
        if (trend_guard) watch_moves(*trend_guard, book, "trend: Binance futures", 0.0, true);
    } else if (&book == bybit_book.get()) {
        if (lag_guard && book.exchange_ts_us && *book.exchange_ts_us && cfg.lag.bybit_max_ms)
            read_late(book.local_ts_us, *book.exchange_ts_us, "Bybit ticker", cfg.lag.bybit_max_ms);
        if (bybit_guard) watch_moves(*bybit_guard, book, "Bybit " + cfg.bybit.category);
    } else if (&book == &reference_book) {
        if (guard) watch_moves(*guard, book, "Binance", cfg.binance.max_book_age_s);
        if (&book == trend_book && trend_guard)  // [futures] off: spot it is
            watch_moves(*trend_guard, book, "trend: Binance", cfg.binance.max_book_age_s, true);
    } else if (&book == &venue_book && arcus_guard) {
        watch_moves(*arcus_guard, book, "Arcus", cfg.arcus.max_book_age_s, true);
    }
    bool changed = false;
    if (auto it = ml_venues_.find(&book); it != ml_venues_.end()) {
        bool ready = book.is_ready();
        auto bid = ready ? book.best_bid() : std::nullopt;
        auto ask = ready ? book.best_ask() : std::nullopt;
        changed = ml->observe(it->second, ml_clock(), bid ? std::optional(bid->first) : std::nullopt, ask ? std::optional(ask->first) : std::nullopt,
                              bid ? std::optional(bid->second) : std::nullopt, ask ? std::optional(ask->second) : std::nullopt);
    }
    // The Arcus levels themselves feed the cancel rules (the book came to the order), whatever the model.
    if (changed || &book == &venue_book) {
        changed_.set();
        if (strategy && gateway && !gateway->orders.empty()) strategy->check_cancel(top_view());
    }
    if (&book == &venue_book) {
        if (gateway) note_top(book);
        mark_trip(book);
    }
    if (show_ladder) print_ladders(book);
}

void Agent::mark_trip(OrderBook& book) {
    // An Arcus frame while a round trip is open: its worst point, and the exit order at the best.
    if (!gateway || !gateway->round_trips->open()) return;
    auto bid = book.best_bid(), ask = book.best_ask();
    if (!bid || !ask) return;
    RoundTrips& trips = *gateway->round_trips;  // the exit side's order: the sell of a long, the buy of a short
    std::string exit_side = trips.trip->side == "BUY" ? "SELL" : "BUY";
    auto it = gateway->orders.find(exit_side);
    OrderPtr order = it != gateway->orders.end() ? it->second : gateway->order();
    bool working = order && order->resting();
    trips.mark(bid->first, ask->first, working ? std::optional(order->side) : std::nullopt,
               working ? std::optional(order->price.to_double()) : std::nullopt);
}

std::optional<ml::Prediction> Agent::ml_prediction() {
    // The ML model now; none while a book it reads is not ready (or older than the max age its
    // pricing allows) or any of its windows is not known all along.
    for (auto& [book, max_age] : ml_books_)
        if (!book->is_ready(max_age)) return std::nullopt;
    auto prediction = ml->predict(ml_clock());
    if (prediction && prediction->skipped != ml_skipped_) {
        std::vector<std::string> gone, back;
        for (const auto& n : prediction->skipped)
            if (std::find(ml_skipped_.begin(), ml_skipped_.end(), n) == ml_skipped_.end()) gone.push_back(n);
        for (const auto& n : ml_skipped_)
            if (std::find(prediction->skipped.begin(), prediction->skipped.end(), n) == prediction->skipped.end()) back.push_back(n);
        ml_skipped_ = prediction->skipped;
        std::vector<std::string> parts;
        if (!gone.empty()) parts.push_back(join(gone, ", ") + " unknown now: held at the last value (the mean if never known)");
        if (!back.empty()) parts.push_back(join(back, ", ") + " back");
        events_->info("--- ml: {}", join(parts, "; "));
    }
    return prediction;
}

std::string Agent::ml_missing() {
    // Why the ML model gives no price now, for the status lines.
    for (auto& [book, max_age] : ml_books_)
        if (!book->is_ready(max_age))
            return book->venue + " book not ready" + (max_age ? std::format(" (or older than {:g} s)", max_age) : "");
    return ml->missing(ml_clock()).value_or("no price");
}

awaitable<void> Agent::ml_tick_loop() {
    // The model's windows slide with time: wake the strategy at least every [ml] eval_interval_ms,
    // market data or not.
    double every = config.ml.eval_interval_ms / 1000;
    for (;;) {
        co_await sleep(every);
        changed_.set();
    }
}

void Agent::watch_moves(Pauses& g, OrderBook& book, const std::string& label, double max_age, bool mid) {
    // A price guard, on every frame of its book (best bid, or mid): a drop pulls a buy, a rise a
    // sell, on the spot. While the book is not ready its frames are skipped, but the window is kept:
    // the Arcus book is rebuilt by design after a sweep has taken most of its known levels, and the
    // first price after the rebuild must still be set against the mean from before the sweep. Only
    // a gap of several windows leaves too old a mean to compare with.
    if (!book.is_ready(max_age)) {
        guard_gaps_.try_emplace(&g, g.clock());
        return;
    }
    if (auto it = guard_gaps_.find(&g); it != guard_gaps_.end()) {
        double since = it->second;
        guard_gaps_.erase(it);
        if (g.clock() - since > 3 * g.window_s) g.reset();
    }
    double price = mid ? *book.mid() : book.best_bid()->first;
    std::optional<Trip> trip;
    if (auto* pg = dynamic_cast<PriceGuard*>(&g)) trip = pg->observe(price);
    else if (auto* tg = dynamic_cast<TrendGuard*>(&g)) trip = tg->observe(price);
    if (!trip) return;
    std::string why = std::format("{} {} {} is {:.2f} bp {} its {} {}", label, mid ? "mid" : "bid", guard_px(price),
                                  std::abs(price / trip->mean - 1) * 1e4, trip->drop() ? "under" : "over", g.reference_of(trip->side),
                                  guard_px(trip->mean));
    // how late the frame was read, from the exchange's stamp on it: each ticker of Binance futures
    // (and Bybit) carries one; the spot top comes from tickers without, the Arcus deltas have none
    bool stamped = book.exchange_ts_us && *book.exchange_ts_us && (&book == futures_book.get() || &book == bybit_book.get());
    std::string seen = stamped ? std::format(" (read {:.0f} ms after the exchange's event)",
                                             static_cast<double>(book.local_ts_us - *book.exchange_ts_us) / 1e3)
                               : "";
    tripped(g, *trip, label, why, seen);
}

void Agent::watch_drift(OrderBook& book) {
    // The drift watch on the futures mid (spot with [futures] off); a side it starts holding gets
    // its line when the markup factor is on.
    if (!book.is_ready()) return;
    auto started = drift->observe(*book.mid());
    double factor = config.drift.markup_factor;
    if (started.empty() || factor == 1) return;
    changed_.set();  // the markup of that side moved
    for (const auto& s : started)
        events_->info("!!! DRIFT futures mid {:+.2f} bp in {}: {} markup x{:g} while it lasts (and {} more)", drift->moved[s] * 1e4,
                      span(drift->window_s), s == "BUY" ? "buy" : "sell", factor, span(drift->hold_s));
}

std::string Agent::drift_report() {
    // Since the last report: how often and how long each side was against the futures drift.
    std::vector<std::string> parts;
    for (auto [s, word] : {std::pair<const char*, const char*> {"BUY", "buys"}, {"SELL", "sells"}}) {
        int64_t episodes = drift->episodes[s];
        double seconds = drift->seconds(s);
        auto then = drift_mark_[s];
        drift_mark_[s] = {episodes, seconds};
        parts.push_back(std::format("{} {} times for {:.0f} s", word, episodes - then.first, seconds - then.second));
    }
    double factor = config.drift.markup_factor;
    std::string verb = factor != 1 ? std::format("markup x{:g}", factor) : "off (markup x1), so only would have widened";
    return join(parts, ", ") + "; " + verb;
}

void Agent::watch_adds(OrderBook& book) {
    // The add guard on the futures mid (spot with [futures] off): a move against a side caps how far
    // an order there may add to the position; the strategy cuts or pulls it itself.
    if (!book.is_ready()) return;
    double price = *book.mid();
    auto trip = add_guard->observe(price);
    if (!trip) return;
    changed_.set();
    if (trip->fresh)
        events_->info("!!! {} futures mid {} is {:.2f} bp {} its {} {} over the last {}: no {} past {:.0f}% of the limit for {}",
                      trip->drop() ? "FALL" : "RALLY", guard_px(price), std::abs(price / trip->mean - 1) * 1e4, trip->drop() ? "under" : "over",
                      add_guard->reference_of(trip->side), guard_px(trip->mean), span(add_guard->window_s), trip->drop() ? "buying" : "selling",
                      config.add_guard.limit_share * 100, span(add_guard->pause_s));
}

void Agent::on_trade(double price, int64_t trade_us, int64_t received_us) {
    // Every Binance spot trade as it is read: the sweep guard, which pulls at once.
    if (lag_guard) read_late(received_us, trade_us, "Binance spot trade");
    last_trade_ = price;
    if (!trades_guard) return;
    auto trip = trades_guard->observe(price);
    if (!trip) return;
    std::string why = std::format("Binance trade {} is {:.2f} bp {} the {} {}", guard_px(price), std::abs(price / trip->mean - 1) * 1e4,
                                  trip->drop() ? "under" : "over", trip->drop() ? "high" : "low", guard_px(trip->mean));
    tripped(*trades_guard, *trip, "Binance trades", why,
            std::format(" (read {:.0f} ms after the trade)", static_cast<double>(received_us - trade_us) / 1e3));
}

void Agent::on_arcus_take(const JsonArray& trades) {
    // [takes]: a frame of the Arcus public trades is one taker order; one of min_usd or more pulls
    // our order on the side it hit and pauses that side (further takes the same way extend it).
    TakeGuard& g = *take_guard;
    const Json& first = trades[0];
    std::string side = jstr(first, "side").value_or("");
    if (side != "BUY" && side != "SELL") return;
    double usd = 0.0;
    try {
        for (const auto& t : trades) usd += to_float(*jget(t, "price")) * to_float(*jget(t, "size"));
    } catch (const std::exception&) {
        return;
    }
    std::string address = lower(truthy(jget(first, "takerAddress")) ? to_str(*jget(first, "takerAddress")) : "");
    auto trip = g.observe(side, usd, address.empty() ? std::nullopt : std::optional(address));
    if (!trip) return;
    std::string taker = (address.empty() ? std::string("?") : address).substr(0, 10);
    std::string why = std::format("Arcus taker {} ${} by {}", lower(side), thousands(usd), taker);
    if (strategy) strategy->pull(trip->side, "take: " + why);  // before anything else, logging included
    if (!trip->fresh) return;
    auto key = std::pair<const Pauses*, std::string> {&g, trip->side};
    if (auto it = resume_timers_.find(key); it != resume_timers_.end()) {
        it->second.cancel();
        resume_timers_.erase(it);
        if (auto previous = g.previous[trip->side])
            events_->info("--- Arcus takes {} pause over: {}, extended {} times", trip->side == "BUY" ? "buying" : "selling",
                          span(std::get<1>(*previous) - std::get<0>(*previous)), std::get<2>(*previous));
    }
    events_->info("!!! TAKE {}: no {} for {}", why, trip->side == "BUY" ? "buying" : "selling", span(g.pause_s));
    std::string trip_side = trip->side;
    resume_timers_[key] = call_later(g.pause_s, [this, gp = &g, trip_side] { resume(gp, trip_side, "Arcus takes"); });
}

void Agent::score_takers(OrderBook& book) {
    // [takes] min_takes: the Arcus mid for the take guard to score the takes by; a taker it turns
    // quiet (its takes pause nothing now) or back is logged.
    TakeGuard& g = *take_guard;
    for (const auto& [taker, n, mean, quiet] : g.mark(book.is_ready() ? book.mid() : std::nullopt))
        events_->info("--- taker {}: the Arcus mid {:+.2f} bp its way {} after its takes (mean of the last {}): {}", taker.substr(0, 10), mean * 1e4,
                      span(g.score_s), n,
                      quiet ? std::format("under {:g} bp, its takes pause nothing now", g.min_move * 1e4)
                            : std::format("{:g} bp or more again, its takes pause again", g.min_move * 1e4));
}

void Agent::on_trades_reset() {
    // Every connection of the trades feed is down: the trades in between are lost.
    last_trade_.reset();
    if (trades_guard) trades_guard->reset();
}

void Agent::tripped(Pauses& g, const Trip& trip, const std::string& label, const std::string& why, const std::string& seen) {
    // A guard tripped: pull the order on the side it threatens, and on a fresh trip log it and time
    // the end of the pause (further trips extend it).
    std::string word = trip.drop() ? "drop" : "rise";
    if (strategy) strategy->pull(trip.side, word + ": " + why);  // before anything else, logging included
    if (!trip.fresh) return;
    auto key = std::pair<const Pauses*, std::string> {&g, trip.side};
    if (auto it = resume_timers_.find(key); it != resume_timers_.end()) {
        // The last pause ended, but its timer has not run yet: it would see this one.
        it->second.cancel();
        resume_timers_.erase(it);
        if (auto previous = g.previous[trip.side])
            events_->info("--- {} {} pause over: {}, extended {} times", label, trip.side == "BUY" ? "buying" : "selling",
                          span(std::get<1>(*previous) - std::get<0>(*previous)), std::get<2>(*previous));
    }
    events_->info("!!! {} {} over the last {}: no {} for {}{}", upper(word), why, span(g.window_s), trip.drop() ? "buying" : "selling",
                  span(g.pause_s), seen);
    std::string side = trip.side;
    resume_timers_[key] = call_later(g.pause_s, [this, gp = &g, side, label] { resume(gp, side, label); });
}

void Agent::resume(Pauses* g, const std::string& side, const std::string& label) {
    // Wake the strategy once a side's pause is over; further moves extend it.
    auto key = std::pair<const Pauses*, std::string> {g, side};
    double left = g->until[side] - g->clock();
    if (left > 0) {
        resume_timers_[key] = call_later(left, [this, g, side, label] { resume(g, side, label); });
        return;
    }
    resume_timers_.erase(key);
    events_->info("--- {} {} pause over: {}, extended {} times", label, side == "BUY" ? "buying" : "selling", span(g->until[side] - g->since[side]),
                  g->extended[side]);
    changed_.set();
}

void Agent::read_late(int64_t received_us, int64_t exchange_us, const std::string& what, double limit) {
    // A message its exchange stamped `exchange_us` and we read at `received_us`: a reading of the lag
    // guard, on the exchange's clock (the host's set against Arcus's by the offset measured to it, so
    // a host clock off by a few hundred ms is not taken for lag).
    double offset = gateway && gateway->clock_offset_ms ? *gateway->clock_offset_ms : 0.0;
    double lag_ms = static_cast<double>(received_us - exchange_us) / 1e3 - offset;
    if (lag_ms >= (limit ? limit : lag_guard->max_ms))  // limit: a feed with its own (Bybit), at least max_ms
        late(lag_ms, std::format("{} read {:.0f} ms after the exchange's time for it", what, lag_ms));
}

void Agent::loop_late(double lag_ms) {
    // The event loop ran `lag_ms` late (the loop watch's timer): everything waited that long.
    if (lag_ms >= lag_guard->max_ms) late(lag_ms, std::format("event loop {:.0f} ms late", lag_ms));
}

void Agent::late(double lag_ms, const std::string& why) {
    // Market data read `lag_ms` late: past the lag guard's max_ms the bot quotes blind. Both orders
    // go at once, and nothing is placed until the readings have been under max_ms for its pause; a
    // fresh pause gets its line, its end another.
    LagGuard& g = *lag_guard;
    auto trips = g.observe(lag_ms);
    if (!trips) return;
    if (strategy) {
        strategy->pull(trips->first.side, "late data: " + why);  // before anything else, logging included
        strategy->pull(trips->second.side, "late data: " + why);
    }
    if (!trips->first.fresh) return;
    if (lag_timer_) {
        // The last pause ended, but its timer has not run yet: it would see this one.
        lag_timer_.cancel();
        if (auto previous = g.previous["BUY"])
            events_->info("--- late data pause over: {}, extended {} times, the latest data read up to {:.0f} ms late",
                          span(std::get<1>(*previous) - std::get<0>(*previous)), std::get<2>(*previous), g.previous_worst);
    }
    events_->info("!!! LATE {} (limit {:.0f} ms): both orders pulled, nothing placed until the data is on time for {}", why, g.max_ms,
                  span(g.pause_s));
    lag_timer_ = call_later(g.pause_s, [this] { lag_resume(); });
}

void Agent::lag_resume() {
    // Wake the strategy once the late-data pause is over; further late readings extend it.
    LagGuard& g = *lag_guard;
    double left = g.until["BUY"] - g.clock();
    if (left > 0) {
        lag_timer_ = call_later(left, [this] { lag_resume(); });
        return;
    }
    lag_timer_ = TimerHandle();
    events_->info("--- late data pause over: {}, extended {} times, the latest data read up to {:.0f} ms late", span(g.until["BUY"] - g.since["BUY"]),
                  g.extended["BUY"], g.worst);
    changed_.set();
}

void Agent::note_gap() {
    // Sample the futures-vs-spot fair gap (at most every GAP_SAMPLE_S).
    auto gap = gap_now();
    if (!gap) {
        gaps_.clear();  // no gap now: a change across the outage would not be "in 2 s"
        return;
    }
    double now = monotonic();
    if (!gaps_.empty() && now - gaps_.back().first < GAP_SAMPLE_S) return;
    gaps_.emplace_back(now, *gap);
    while (gaps_.size() > 1 && gaps_[1].first <= now - GAP_WINDOW_S) gaps_.pop_front();
}

std::optional<double> Agent::gap_now() const {
    if (!futures_fair_bid || !spot_fair_bid) return std::nullopt;
    return (*futures_fair_bid / *spot_fair_bid - 1.0) * 1e4;
}

std::optional<double> Agent::gap_change(std::optional<double> gap) const {
    // How much the gap moved since GAP_WINDOW_S ago (the oldest sample kept), none without one.
    if (!gap || gaps_.empty() || monotonic() - gaps_.front().first < GAP_WINDOW_S * 0.9) return std::nullopt;
    return *gap - gaps_.front().second;
}

void Agent::note_top(OrderBook& book) {
    auto bid = book.best_bid(), ask = book.best_ask();
    if (!bid || !ask) return;
    double now = monotonic();
    if (!tops_.empty()) {
        auto& [t, b, bs, a, as] = tops_.back();
        if (b == bid->first && bs == bid->second && a == ask->first && as == ask->second) return;
    }
    tops_.emplace_back(now, bid->first, bid->second, ask->first, ask->second);
    while (tops_.size() > 1 && std::get<0>(tops_[1]) <= now - 1.0) tops_.pop_front();
}

void Agent::on_fill(const OrderPtr& order, const Decimal& size) {
    // One line of context after every FILL line: the fill against the fair bids (edge: how far the
    // fair mid is on our side of the fill price, a fair bid plus half a tick being the fair mid), how
    // far futures have pulled away from spot for this side, the Arcus top now and TOP_BEFORE_FILL_S
    // earlier, each guarded price against its guard's mean, and how long the order stood.
    double price = order->price.to_double();
    double sign = order->side == "BUY" ? 1.0 : -1.0;
    double half_tick = gateway->market.tick_at(order->price).to_double() / 2;
    std::optional<ml::Prediction> prediction;
    std::optional<double> fair;
    if (ml) {
        prediction = ml_prediction();
        if (prediction) fair = prediction->fair_bid;
    } else {
        fair = fair_bid;
    }
    std::vector<std::pair<std::string, std::optional<double>>> fairs {{"fair bid", fair}};
    for (const auto& [name, shadow, p] : shadow_fairs()) fairs.emplace_back(name, shadow);
    std::vector<std::string> parts;
    for (const auto& [name, value] : fairs)
        if (value) parts.push_back(std::format("{} {:.2f} (edge {:+.2f} bp)", name, *value, sign * ((*value + half_tick) / price - 1.0) * 1e4));
    if (parts.empty()) parts.push_back("no fair bid");
    if (prediction)
        parts.push_back(std::format("ml {:+.2f} bp over {} {:.10g}", prediction->value * 1e4, ml->base_text(), prediction->prev));
    if (auto note = gap_note(gap_now(), gap_change(gap_now()), order->side)) parts.push_back(*note);
    if (auto note = drift_text(*drift, order->side, config.drift.markup_factor)) parts.push_back(*note);
    auto bid = venue_book.best_bid(), ask = venue_book.best_ask();
    if (bid && ask) parts.push_back(std::format("arcus {} x {:g} / {} x {:g}", px(bid->first), bid->second, px(ask->first), ask->second));
    double then = monotonic() - TOP_BEFORE_FILL_S;
    for (auto it = tops_.rbegin(); it != tops_.rend(); ++it) {
        if (std::get<0>(*it) <= then) {
            auto [t, b, bs, a, as] = *it;
            parts.push_back(std::format("{:.0f} ms before: {} x {:g} / {} x {:g}", TOP_BEFORE_FILL_S * 1000, px(b), bs, px(a), as));
            break;
        }
    }
    std::vector<std::string> moves;
    for (auto [label, g, book, mid] : {std::tuple<const char*, PriceGuard*, OrderBook*, bool> {"binance bid", guard.get(), &reference_book, false},
                                       {"futures bid", futures_guard.get(), futures_book.get(), false},
                                       {"bybit bid", bybit_guard.get(), bybit_book.get(), false},
                                       {"arcus mid", arcus_guard.get(), &venue_book, true}}) {
        if (g == nullptr || book == nullptr || !book->is_ready()) continue;
        auto mean = g->mean();
        if (!mean) continue;
        double now = mid ? *book->mid() : book->best_bid()->first;
        moves.push_back(std::format("{} {:+.2f}", label, (now / *mean - 1.0) * 1e4));
    }
    if (!moves.empty()) parts.push_back("vs guard means " + join(moves, ", ") + " bp");
    if (last_trade_ && trades_guard) {
        if (auto extremes = trades_guard->extremes()) {
            auto [high, low] = *extremes;
            parts.push_back(std::format("last Binance trade {:+.2f} bp vs the high of the last {}, {:+.2f} vs the low",
                                        (*last_trade_ / high - 1.0) * 1e4, span(trades_guard->window_s), (*last_trade_ / low - 1.0) * 1e4));
        }
    }
    std::vector<std::string> late_reads;  // how late the newest message of each feed was read, from the exchange's stamp
    if (futures_book && futures_book->exchange_ts_us && *futures_book->exchange_ts_us)
        late_reads.push_back(std::format("futures ticker {:.0f} ms after its event",
                                         static_cast<double>(futures_book->local_ts_us - *futures_book->exchange_ts_us) / 1e3));
    if (trades_feed && trades_feed->last_trade_us)
        late_reads.push_back(std::format("spot trade {:.0f} ms after it", static_cast<double>(*trades_feed->last_received_us - *trades_feed->last_trade_us) / 1e3));
    if (!late_reads.empty()) parts.push_back("last read: " + join(late_reads, ", "));
    parts.push_back(std::format("order stood {:.0f} ms", (monotonic() - order->placed_at) * 1000));
    events_->info("--- fill context {} {} @ {}: {}", order->side, fmt(size), fmt(order->price), join(parts, "; "));
}

MarketView Agent::top_view() {
    MarketView v;
    v.venue = venue_book.is_ready(config.arcus.max_book_age_s) ? venue_book.top(top_levels_) : nullptr;
    if (ml) {
        auto prediction = ml_prediction();
        if (prediction) v.fair_bid = prediction->fair_bid;
        return v;
    }
    v.fair_bid = fair_bid;
    v.futures_fair_bid = futures_fair_bid;
    v.spot_fair_bid = spot_fair_bid;
    return v;
}

std::vector<std::tuple<std::string, std::optional<double>, Pricer*>> Agent::shadow_fairs() const {
    // The fair bids kept for the logs only: (name, fair, its pricer).
    std::vector<std::tuple<std::string, std::optional<double>, Pricer*>> shadows;
    if (futures_pricer && futures_pricer != pricer) shadows.emplace_back("futures fair bid", futures_fair_bid, futures_pricer.get());
    if (spot_pricer != pricer) shadows.emplace_back("spot fair bid", spot_fair_bid, spot_pricer.get());
    return shadows;
}

void Agent::log_tob(OrderBook& book, const std::string& label) {
    // [agent] tob_lines: "ARCUS: 82081.8 exchange ... now ..." when the book's best bid price is not
    // the one last written. The exchange's time of the frame that moved it (UTC) when the frame has
    // one: not a Binance spot bookTicker, not an Arcus depth update (only its snapshot is stamped).
    auto top = book.best_bid();
    if (!top) return;
    if (auto it = tob_last_.find(&book); it != tob_last_.end() && it->second == top->first) return;
    tob_last_[&book] = top->first;
    int64_t now = now_us();
    std::optional<int64_t> stamp = book.bbo() ? std::nullopt : book.exchange_ts_us;  // the overlay (bookTicker) has no stamp
    if (stamp && *stamp)
        events_->info("{}: {} exchange {} now {} (+{:.1f} ms)", label, px(top->first), utc_time_us(*stamp), utc_time_us(now),
                      static_cast<double>(now - *stamp) / 1e3);
    else
        events_->info("{}: {} exchange n/a now {}", label, px(top->first), utc_time_us(now));
}

void Agent::update_futures_fair(bool do_sample) {
    // The fair Arcus bid priced off Binance futures: mid x the basis to the Arcus bid. None while
    // either book is not ready to price from or the pricer warms up: priced off futures, that pulls
    // the quote.
    if (!futures_book->is_ready(config.futures.max_book_age_s)) {
        futures_fair_bid.reset();
        futures_mid_.reset();  // so the first fresh ticker prices again, even at the old mid
        return;
    }
    double mid = *futures_book->mid();
    if (do_sample) sample(*futures_pricer, mid);
    futures_fair_bid = futures_pricer->fair(mid);
}

void Agent::sample(Pricer& p, double reference_price) {
    // One basis sample: the Arcus bid (without our own buy) over `reference_price`. A time-weighted
    // basis pauses while the Arcus book is down or has not changed for BASIS_SAMPLE_MAX_AGE_S, so
    // its last ratio does not count for that time.
    bool fresh = venue_book.is_ready(config.arcus.max_book_age_s) && (!p.timed() || venue_book.age() <= BASIS_SAMPLE_MAX_AGE_S);
    auto bid = fresh ? venue_bid() : std::nullopt;
    if (bid) p.observe(*bid, reference_price);
    else if (p.timed()) p.pause();
}

void Agent::update_spot_fair(OrderBook& updated) {
    // Naive pricing off Binance spot: fair Arcus bid = Binance bid x mean(Arcus bid / Binance bid),
    // or Binance mid x the time-weighted basis. With the window of samples, each Arcus book update
    // adds one; the time-weighted basis takes a sample on every update of either book.
    Pricer& p = *spot_pricer;
    if (!reference_book.is_ready(config.binance.max_book_age_s)) {
        spot_fair_bid.reset();
        return;
    }
    bool timed = p.timed();
    double ref_price = timed ? *reference_book.mid() : reference_book.best_bid()->first;
    if (&updated == &venue_book || timed) sample(p, ref_price);
    spot_fair_bid = p.fair(ref_price);
}

std::optional<double> Agent::venue_bid() {
    // Arcus best bid without our own resting buy, so we never price off ourselves.
    const std::vector<Level>& levels = venue_book.top(2)->bids;
    auto own = strategy ? strategy->own_in_book("BUY", &levels) : std::nullopt;
    if (!own) return levels.empty() ? std::nullopt : std::optional(levels[0].first);
    return others_best(levels, own->first, own->second);
}

void Agent::print_ladders(OrderBook& updated) {
    // Both books side by side; called on every update of either one.
    int depth = config.agent.depth;
    std::vector<std::string> header {local_time_us(), updated.venue + " " + book_state(updated)};
    header.push_back(std::format("seq {}", updated.seq));
    if (updated.synced && updated.exchange_ts_us && *updated.exchange_ts_us)
        header.push_back(std::format("lat {:.1f}ms", static_cast<double>(updated.local_ts_us - *updated.exchange_ts_us) / 1e3));
    if (auto bbo = updated.bbo(); updated.synced && bbo) header.push_back(std::format("bbo +{}", bbo->seq - updated.seq));  // top is this many update ids ahead
    auto ref_mid = reference_book.mid(), venue_mid = venue_book.mid();
    if (ref_mid && *ref_mid && venue_mid) header.push_back("basis " + basis_text((*venue_mid / *ref_mid - 1.0) * 1e4));
    header.push_back(pricing_summary());
    if (gateway) {
        std::vector<std::string> orders;
        for (const auto& o : gateway->working()) orders.push_back(o->str());
        header.push_back(std::format("position {}, order {}", fmt(gateway->position()), orders.empty() ? "none" : join(orders, ", ")));
    }
    std::string ladders = format_ladders(*reference_book.top(depth), *venue_book.top(depth),
                                         std::pair {std::format("{} {} [{}]", reference_book.venue, reference_book.symbol, book_state(reference_book)),
                                                    std::format("{} {} [{}]", venue_book.venue, venue_book.symbol, book_state(venue_book))});
    stdout_write(join(header, "  ") + "\n" + ladders + "\n\n");
}

awaitable<void> Agent::market_data_loop() {
    for (;;) {
        co_await changed_.wait();
        changed_.clear();
        view = build_view();
        on_market_data(*view);
    }
}

MarketViewPtr Agent::build_view() {
    const Config& cfg = config;
    int depth = cfg.agent.depth;
    auto v = std::make_shared<MarketView>();
    // the spot top only for its best bid and ask: the quote lines and the status line
    v->reference = reference_book.is_ready(cfg.binance.max_book_age_s) ? reference_book.top(1) : nullptr;
    v->venue = venue_book.is_ready(cfg.arcus.max_book_age_s) ? venue_book.top(depth) : nullptr;
    v->futures = futures_book && futures_book->is_ready() ? futures_book->top(1) : nullptr;
    v->bybit = bybit_book && bybit_book->is_ready() ? bybit_book->top(1) : nullptr;
    if (ml) {  // the model's price as of now: its windows slide with time
        auto prediction = ml_prediction();
        fair_bid = prediction ? std::optional(prediction->fair_bid) : std::nullopt;
        v->fair_bid = fair_bid;
        if (prediction) v->model_bps = prediction->value * 1e4;
        return v;
    }
    auto fair = fair_bid;
    // Woken by an order event or a guard, not a frame: the book priced from may have gone stale since.
    double max_age = price_book == futures_book.get() ? cfg.futures.max_book_age_s : cfg.binance.max_book_age_s;
    if (fair && !price_book->is_ready(max_age)) fair.reset();
    v->fair_bid = fair;
    v->futures_fair_bid = futures_fair_bid;
    v->spot_fair_bid = spot_fair_bid;
    auto gap = gap_now();
    v->fair_gap_bps = gap;
    v->fair_gap_change_bps = gap_change(gap);
    return v;
}

void Agent::on_market_data(const MarketView& v) {
    // Strategy hook: called after book or order changes with the latest view. When either book or
    // the fair bid is none the strategy pulls its quote.
    if (strategy) strategy->evaluate(v);
    if (quotes) quotes->update(v);
}

std::string Agent::pricing_summary() {
    if (ml) {
        auto prediction = ml_prediction();
        if (!prediction) return "ml fair bid: n/a (" + ml_missing() + ")";
        std::string text = std::format("ml fair bid {}: {:+.2f} bp over {} {}", px(py_round(prediction->fair_bid, 8)), prediction->value * 1e4,
                                       ml->base_text(), px(py_round(prediction->prev, 8)));
        if (auto bid = venue_book.best_bid())
            text += std::format(", arcus bid {} ({:+.2f} bp)", px(bid->first), (bid->first / prediction->fair_bid - 1.0) * 1e4);
        return text;
    }
    if (!fair_bid) {
        if (static_cast<int64_t>(pricer->samples()) < pricer->min_samples)
            return std::format("fair bid: warming up {}/{}", pricer->samples(), pricer->min_samples);
        return "fair bid: n/a";
    }
    std::string text = "fair bid " + px(py_round(*fair_bid, 8));
    if (auto bid = venue_book.best_bid())  // where the actual Arcus best bid sits relative to the estimate
        text += std::format(" arcus {} ({:+.2f} bp)", px(bid->first), (bid->first / *fair_bid - 1.0) * 1e4);
    return text;
}

// --------------------------------------------------------------------- status

awaitable<void> Agent::status_loop() {
    for (;;) {
        co_await sleep(config.agent.status_interval_s);
        try {
            log_status();
        } catch (const std::exception& err) {  // a status line must never stop trading
            log_->error("status report failed: {}", err.what());
        }
    }
}

awaitable<void> Agent::health_loop() {
    double every = config.agent.health_interval_s;
    loop_watch->cpu();  // the CPU share counts from here
    for (;;) {
        co_await sleep(every);
        try {
            events_->info("--- health over the last {}: {}", span(every), health());
            events_->info("--- drift over the last {}: {}", span(every), drift_report());
        } catch (const std::exception& err) {  // a health line must never stop trading
            log_->error("health report failed: {}", err.what());
        }
    }
}

std::string Agent::health() {
    // The event loop's lag, the CPU share and how late each stamped stream was read, since the last
    // call: where the time goes when the bot runs behind.
    std::vector<std::string> parts;
    if (auto lag = loop_watch->lags.take()) parts.push_back("event loop lag " + *lag);
    if (auto cpu = loop_watch->cpu()) parts.push_back(std::format("cpu {:.0f}% of a core", *cpu * 100));
    for (auto& feed : feeds)
        if (auto* futures = dynamic_cast<BinanceFuturesTickerFeed*>(feed.get()))
            if (auto late_text = futures->delays.take()) parts.push_back("Binance futures tickers read " + *late_text + " after their event");
    if (trades_feed)
        if (auto late_text = trades_feed->delays.take()) parts.push_back("Binance spot trades read " + *late_text + " after the trade");
    if (gateway)
        if (auto late_text = gateway->delays.take()) parts.push_back("Arcus order events read " + *late_text + " after the exchange's stamp");
    if (loop_watch->stalls)
        parts.push_back(std::format("{} loop stalls of {:.0f} ms or more since the start", loop_watch->stalls, loop_watch->stall_ms));
    if (lag_guard) {
        double now = lag_guard->clock();
        double paused = lag_guard->paused_for(now);
        if (lag_mark_ && now > lag_mark_->first && paused > lag_mark_->second) {
            double share = (paused - lag_mark_->second) / (now - lag_mark_->first);
            parts.push_back("nothing placed for late data " + percent(share, 1) + " of the time");
        }
        lag_mark_ = std::pair {now, paused};
    }
    return parts.empty() ? "nothing measured yet" : join(parts, "; ");
}

void Agent::log_status() {
    auto v = build_view();
    for (auto& feed : feeds) log_->info("{}", status_line(*feed));
    if (trades_feed) log_->info("{}", trades_feed->status());
    log_->info("basis arcus/binance mid: {}", basis_text(v->basis_bps()));
    std::string pricing = pricing_summary();
    if (pricer)
        if (auto ratio = pricer->ratio())
            pricing += std::format(", ratio {:.8f} ± {} over {} samples", *ratio, std_text(*pricer), pricer->samples());
    log_->info("pricing: {}", pricing);
    if (ml)
        if (auto prediction = ml_prediction()) {
            std::vector<std::string> features;
            for (const auto& [name, x] : prediction->features) features.push_back(std::format("{} {:+.3f}", name, x * 1e4));
            log_->info("ml features (bp): {}", join(features, ", "));
        }
    for (const auto& [name, shadow, p] : shadow_fairs()) {  // the other reference's fair, for comparison
        if (!shadow) {
            log_->info("{}: n/a ({}/{} samples)", name, p->samples(), p->min_samples);
        } else {
            std::string vs = fair_bid ? std::format(" ({:+.2f} bp vs fair bid)", (*shadow / *fair_bid - 1.0) * 1e4) : "";
            auto ratio = p->ratio();
            log_->info("{} {}{}, basis {} ± {}", name, px(py_round(*shadow, 8)), vs, ratio ? std::format("{:.8f}", *ratio) : "None", std_text(*p));
        }
    }
    for (auto [name, g] : {std::pair<const char*, Pauses*> {"guard", guard.get()}, {"futures guard", futures_guard.get()},
                           {"trades guard", trades_guard.get()}, {"bybit guard", bybit_guard.get()}, {"arcus guard", arcus_guard.get()},
                           {"trend guard", trend_guard.get()}, {"take guard", take_guard.get()}}) {
        if (g == nullptr) continue;
        std::vector<std::string> pausing;
        for (auto [s, verb] : {std::pair<const char*, const char*> {"BUY", "buying"}, {"SELL", "selling"}})
            if (g->paused(s)) pausing.push_back(std::format("no {} now", verb));
        if (g == take_guard.get() && take_guard->scoring())
            pausing.push_back(std::format("{} quiet takers, {} of their takes paused nothing", take_guard->quiet.size(), take_guard->skipped));
        std::string tail;
        for (const auto& p : pausing) tail += ", " + p;
        log_->info("{}: {} drops pulled buys, {} rises pulled sells{}", name, g->trips["BUY"], g->trips["SELL"], tail);
    }
    if (lag_guard) {
        int64_t n = lag_guard->trips["BUY"];
        log_->info("lag guard: {} {} for late data, {} in all{}", n, n == 1 ? "pause" : "pauses", span(lag_guard->paused_for()),
                   lag_guard->paused("BUY") ? ", nothing placed now" : "");
    }
    if (gateway) log_->info("trading: {}", gateway->status());
    if (quotes) log_->info("{}", quotes->status());
}

}  // namespace nmm
