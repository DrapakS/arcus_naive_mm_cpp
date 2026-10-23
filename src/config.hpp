// Configuration: the defaults below, optionally overridden by a TOML file and by
// --set section.key=value. Strict on types (bool("false") is True, so casting
// would hide mistakes), and every value checked before the bot starts.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nmm {

namespace ml {
struct Params;
}

// A value of a list or a table of the config, kept as the file gave it: the
// checks of what it holds come later (validate), with the value in the message.
struct ConfigValue {
    enum class Kind { String, Integer, Float, Bool, Other };
    Kind kind = Kind::Other;
    std::string text;  // a string's
    double number = 0.0;  // a number's (an integer's as a double too)
    std::string repr;  // as Python's repr() prints it
    bool is_string() const { return kind == Kind::String; }
    bool is_number() const { return kind == Kind::Integer || kind == Kind::Float; }
    static ConfigValue string(std::string s);
    static ConfigValue integer(int64_t n);
    static ConfigValue floating(double x);
};
using ConfigList = std::vector<ConfigValue>;
using ConfigTable = std::vector<std::pair<std::string, ConfigValue>>;  // in file order

// The strings of a list (validate has made sure there is nothing else in it).
std::vector<std::string> strings(const ConfigList& list);
// The numbers of a table, in order.
std::vector<std::pair<std::string, double>> numbers(const ConfigTable& table);

struct BinanceConfig {
    std::string symbol = "BTCUSDT";
    std::string ws_url = "wss://stream.binance.com:9443";
    std::string rest_url = "https://api.binance.com";
    int snapshot_limit = 1000;  // REST depth levels; also the local book cap
    int update_speed_ms = 100;  // 100 or 1000
    bool book_ticker = true;  // overlay the real-time best bid/ask from <symbol>@bookTicker
    double stale_timeout_s = 30.0;  // reconnect after this long without a message; 0 = off
    double max_book_age_s = 2.0;  // book older than this is not ready; 0 = off
    int connections = 2;  // same stream over this many connections; the first copy of each update wins
};

struct ArcusConfig {
    std::string market = "BTC-USD";
    std::string ws_url = "wss://api.arcus.xyz/v1/ws";  // testnet: wss://api.testnet.arcus.xyz/v1/ws
    std::string rest_url = "https://api.arcus.xyz";  // testnet: https://api.testnet.arcus.xyz
    // snapshot depth per side, 1..100; deltas cover every price, so a deeper
    // snapshot means fewer snapshot refreshes when the market moves
    int n_levels = 100;
    // A quiet book sends nothing, so silence is not staleness here: liveness
    // comes from websocket ping/pong. Both checks are off by default.
    double stale_timeout_s = 0.0;
    double max_book_age_s = 0.0;
    int connections = 2;  // same stream over this many connections; the first copy of each update wins
    // Assets the Binance and Bybit symbols may quote instead of the market's own: a market priced
    // off its underlying, as GLD-USD (a gold ETF share) off gold, ["XAU", "XAUT", "PAXG"]. The
    // basis carries the ratio of the prices (a GLD share is ~0.092 oz). Empty: the market's asset only.
    ConfigList feed_assets;
};

struct PricingConfig {
    // "basis": the fair Arcus bid is a reference price times its basis to Arcus (the keys below).
    // "ml": the model of [ml] over the features of research/ml_pricing.ipynb; nothing of the basis
    // pricing is computed then (and with "basis", nothing of the model).
    std::string model = "basis";
    // What the fair Arcus bid is priced off: "spot" (Binance spot) or "futures"
    // (Binance USD-M futures, which move first; needs [futures] enabled).
    std::string reference = "spot";
    // basis_s = 0: fair = spot bid x mean(Arcus bid / spot bid) (with "futures":
    // futures mid x mean(Arcus bid / futures mid)) over the last `window` samples,
    // one per Arcus book update. basis_s > 0: fair = reference mid x a time-weighted
    // moving average of Arcus bid / reference mid with this time constant (s), fed
    // by every update of either book.
    double basis_s = 0.0;
    int window = 1000;
    int min_samples = 100;  // no estimate before this many samples
};

struct MlConfig {
    // pricing.model = "ml" (ml.cpp): the model predicts P_next / P_base - 1, P_next the time-weighted
    // mean Arcus `target_price` over the next `target_window_s`, P_base by `target_base`: "now" the
    // price now, "prev" its mean over the last `target_window_s`; fair = P_base * (1 + prediction),
    // less how far the target price stands above the Arcus bid. The other keys are the notebook's
    // parameters of the same names: keep them as the model was trained with.
    // `weights`: feature -> weight, the notebook's names plus "intercept"; fractions, not bp.
    ConfigTable weights;
    // "linear": the weights above; "lightgbm": the LightGBM model in `lightgbm_model` (LightGBM's
    // text format, or the pickle of a Booster or an LGBMRegressor, read without unpickling it).
    // `features`: its columns in order, the notebook's names; needed when the model was fit on an
    // array (it calls them Column_0...).
    std::string kind = "linear";
    std::string lightgbm_model;
    ConfigList features;
    // The model was fit on scaled features (sklearn StandardScaler): the model reads
    // (feature - mean) / scale, [ml.mean] and [ml.scale] the scaler's mean_ and scale_.
    bool scaled = false;
    ConfigTable mean;
    ConfigTable scale;
    double target_window_s = 3.0;  // TARGET_WINDOW_S
    // "now": P_next / p(t) - 1; "prev": P_next / P_prev - 1; off the futures price f(t):
    // "futures", the model predicts it whole, "basis", it predicts it less B(t) - 1
    std::string target_base = "now";
    double baseline_tau_s = 30.0;  // BASELINE_TAU_S: the time constant of B(t) for target_base "basis"
    // A feature unknown now keeps its last known value (its scaler mean, 0 without a scaler, if
    // never known) and the model prices on; the features in `required` and the prices of the fair
    // itself still have to be known. false: no price until every feature is known.
    bool skip_missing = false;
    ConfigList required;
    std::string target_price = "mid";  // TARGET_PRICE: "mid", "bid" or "ask"
    double max_gap_s = 10.0;  // MAX_GAP_S of the Binance and Bybit books: no frame this long, the price is unknown
    double arcus_max_gap_s = 60.0;  // MAX_GAP_S["arcus"]
    // The windows slide with no market data: re-price at least this often (the notebook's grid
    // is 100 ms); 0 = only on market data and order events
    double eval_interval_ms = 100.0;
    // A prediction is reused for this long (ms) rather than worked out again: in a burst of futures
    // tickers the model ran twice a ticker (the cancel check and the strategy loop), and the
    // Python bot's loop stood 100-150 ms (live ETH 19 Oct). The shortest window is 1 s; 0 = on every ask.
    double reuse_ms = 1.0;
};

struct TradingConfig {
    std::string mode = "off";  // off | quote (prices only, no orders) | paper (simulated fills) | live
    double markup = 0.0005;  // fraction: buy at fair*(1-markup), sell at (fair+tick)*(1+markup)
    // "long": buy first, then sell what was bought; "short": sell first, then buy it back;
    // "both": a buy and a sell at once, order_size each, the position either way up to
    // max_position_usd (each order cut to keep within it; a side at the limit is not quoted
    // until the position comes back)
    std::string direction = "long";
    double max_position_usd = 1000.0;  // direction = "both": the largest position either way, dollars
    // direction = "both": a side held at the limit comes back once the position on that side is
    // under this share of max_position_usd (0.95: under $950 of $1000)
    double max_position_resume = 0.95;
    // A market with trading hours (Arcus RWA perps): outside the session the markup of both sides
    // is this many times more; 1 = the same at all hours
    double off_hours_markup_factor = 1.0;
    // direction = "both": the position as a share u of max_position_usd skews the quotes, maker
    // only: the opening side takes markup * (1 + skew_open * u), the closing side its markup *
    // (1 - skew_close * u); 0 = no skew
    double skew_open = 0.0;
    double skew_close = 0.0;
    // Passive exit urgency (no taker, no stop-loss): a position held exit_after_s or more, or
    // exit_loss (fraction) under water at the Arcus mid, gets its closing order at the best price
    // on its side (joined, never crossed) with no markup; 0 = off
    double exit_after_s = 0.0;
    double exit_loss = 0.0;
    // The closing order (the sell of a long, the buy of a short) takes this much less markup:
    // 0.3 = 30% less; 0 = the same
    double close_markup_cut = 0.0;
    double tol = 0.0002;  // fraction: cancel when the order is this far off its target price
    double tol_away = 0.0;  // the same when the target moved away from the order (it sits deeper); 0 = tol
    // The closing order's tol_away: a sell left above a target that fell is re-placed past this; 0 = tol
    double close_tol_away = 0.0;
    double order_size = 0.001;  // buy size in base units (BTC); sells close the position
    // never better than the others' best price on the order's side, and cancel the
    // moment the book comes to the order (levels ahead vanish, or it is left alone on top)
    bool never_lead = false;
    // never_lead: after the book came to our order and it was cancelled, no order on that side at
    // its price or better for this long; a target there goes one tick behind it instead. 0 = off.
    double came_hold_ms = 0.0;
    double dms_timeout_s = 60.0;  // live: dead man's switch deadline, refreshed every half; 0 = off
    int cancel_lanes = 1;  // live: extra connections every cancel is also sent on; 0 = main only
    double presign_cancel_s = 1.0;  // live: cancel signed ahead, kept at most this old; 0 = sign when sent
    // live: a connection to Arcus whose cancel replies (on main also our order events) take a median
    // of more than this over a minute is reconnected, one at a time, each at most every 5 min; 0 = never
    double slow_reconnect_ms = 50.0;
    std::string orders_log;  // also write order events to this file as they happen; "" = console only
};

struct GuardConfig {
    // Sharp Binance moves: when its best bid is more than `drawdown` (fraction) under its mean over
    // the last `window_ms` (time-weighted), a working buy is cancelled at once and no buy is placed
    // for `pause_ms`; that far over the mean, the same for sells. 0 = off.
    double window_ms = 1000.0;
    double drawdown = 0.0001;
    double pause_ms = 100.0;
};

struct FuturesConfig {
    // Binance USD-M futures best bid/ask (bookTicker), a second input of the price guard: futures
    // tend to move before spot. drawdown 0 keeps the feed (shown on the price lines) without the guard.
    bool enabled = true;
    std::string symbol = "BTCUSDT";
    std::string ws_url = "wss://fstream.binance.com";
    double stale_timeout_s = 30.0;  // reconnect after this long without a message; 0 = off
    int connections = 2;  // same stream over this many connections; the first copy of each ticker wins
    double max_book_age_s = 2.0;  // when the fair is priced off futures: older than this is no price; 0 = off
    double window_ms = 1000.0;
    double drawdown = 0.0001;
    double pause_ms = 100.0;
};

struct TradesConfig {
    // Binance spot trades (<symbol>@aggTrade of [binance] symbol), the fastest input of the price
    // guard: a trade more than `drawdown` under the highest trade of the last `window_ms` pulls a
    // buy and pauses buys until `pause_ms` after the last trade that far down; over the lowest,
    // sells. drawdown 0 keeps the feed (its delays in the health line) without the guard.
    bool enabled = true;
    std::string ws_url;  // "" = [binance] ws_url
    double stale_timeout_s = 60.0;  // reconnect after this long without a trade; 0 = off
    int connections = 2;  // same stream over this many connections; the first copy of each trade wins
    double window_ms = 300.0;
    double drawdown = 0.00003;  // 0.3 bp
    double pause_ms = 200.0;
};

struct BybitConfig {
    // Bybit V5 best bid/ask (orderbook.1), one more input of the price guard: `category` "linear"
    // (the USDT perpetual) or "spot". drawdown 0 keeps the feed without the guard.
    bool enabled = true;
    std::string category = "linear";
    std::string symbol = "BTCUSDT";
    std::string ws_url = "wss://stream.bybit.com";
    double stale_timeout_s = 30.0;  // reconnect after this long without a message; 0 = off
    int connections = 2;  // same stream over this many connections; the first copy of each snapshot wins
    double window_ms = 1000.0;
    double drawdown = 0.0001;
    double pause_ms = 100.0;
};

struct OkxConfig {
    // OKX V5 best bid/ask (bbo-tbt, pushed every 10 ms) of the perpetual `inst_id`, one more input of
    // the price guard: same rule as [guard] with its own window (ms), drawdown and pause; drawdown 0
    // keeps the feed (shown on the price lines) without the guard. Off unless enabled.
    bool enabled = false;
    std::string inst_id = "BTC-USDT-SWAP";
    std::string ws_url = "wss://ws.okx.com:8443";  // port 443 works too: wss://ws.okx.com
    double stale_timeout_s = 30.0;  // reconnect after this long without a message; 0 = off
    int connections = 2;  // same stream over this many connections; the first copy of each snapshot wins
    double window_ms = 200.0;
    double drawdown = 0.00002;
    double pause_ms = 300.0;
};

struct CTraderConfig {
    // The broker's spot price over the cTrader FIX API (QUOTE session; IC Markets: US500, USTEC,
    // XAUUSD), one more input of the price guard: same rule as [guard] on the MID, with its own
    // window (ms), drawdown and pause; drawdown 0 keeps the feed (on the status lines) without the
    // guard. The FIX settings (host, SenderCompID, password) are in `credentials`, a TOML file only its
    // owner reads: one login per bot, the broker gives a login one price session. Off unless enabled.
    bool enabled = false;
    std::string symbol;  // the broker's name: US500, USTEC, XAUUSD ("NVDA" also finds "NVDA.US")
    std::string credentials = "~/.config/naive_mm/ctrader_fix.toml";
    double heartbeat_s = 30.0;  // HeartBtInt: a session silent for twice this is built again
    double window_ms = 300.0;
    double drawdown = 0.00005;
    double pause_ms = 200.0;
};

struct TakesConfig {
    // Arcus taker waves: a taker order of `min_usd` or more in the Arcus public trades pulls our
    // order on the side it hit and no order goes there for `pause_ms` after the last such take.
    // pause_ms 0 = off.
    double min_usd = 0.0;
    double pause_ms = 0.0;
    // Quiet takers: each take is scored by the Arcus mid `score_ms` after it against the mid just
    // before it; a taker with `min_takes` takes scored whose mean is under `min_move` pauses
    // nothing, and is still scored. min_takes 0 = every take counts.
    int min_takes = 0;
    double min_move = 0.00002;
    double score_ms = 1000.0;
};

struct ArcusGuardConfig {
    // The Arcus price itself: when the Arcus mid is more than `drawdown` over its mean over the
    // last `window_ms`, a working sell is cancelled and no sell is placed for `pause_ms`; that far
    // under, the same for buys. 0 = off.
    double window_ms = 3000.0;
    double drawdown = 0.00005;
    double pause_ms = 300.0;
};

struct OpenGuardConfig {
    // Moments the underlying market opens, weekly, local time in `tz`: "Sun 18:00", "Mon-Thu 18:00",
    // "Mon-Fri 09:30". From `before_s` before to `after_s` after each, no order that opens or adds
    // to the position is placed (with `limit_share` > 0, none past that share of max_position_usd);
    // the order that shrinks the position is quoted as usual. [] = off.
    ConfigList opens;
    std::string tz = "America/New_York";
    double before_s = 60.0;
    double after_s = 900.0;
    double limit_share = 0.0;
};

struct OpenHaltConfig {
    // Moments trading stops outright, weekly, local time in `tz` (as [open_guard]): from `before_s` before to
    // `after_s` after each, every order is pulled, the closing one too, and none is placed. For the US stock
    // open, "Mon-Fri 09:30": NVDA, 19 Oct 13:30 UTC, -50 bp in the first 30 s, 284 trades in them; its
    // closing sell was hit at 217.15 against a 218.54 buy. [open_guard] still holds adding after it. [] = off.
    ConfigList opens;
    std::string tz = "America/New_York";
    double before_s = 60.0;
    double after_s = 120.0;
    // true: within a window the closing order still goes, as many as the position and not past zero;
    // only opening or adding to it stops (an order already open that would is pulled).
    bool closing = false;
};

struct PortfolioConfig {
    // Inventory by asset class across the bots of a host, each still its own process on its own market:
    // every bot writes its position to a file in `dir`/`group` and reads the others' every interval_ms. The
    // class exposure (weight x position x mark over its markets, ours included; weight = the market's risk in
    // BTC units, its 1-min volatility over BTC's) over limit_usd is u (-1..1); an order adding to the lean
    // takes markup x (1 + factor u), one taking it back x (1 - factor u), on top of the bot's own skew; shift_bp
    // moves both quotes against the lean by shift_bp x u (the class long: both lower; 0 = off). A file
    // not rewritten for stale_s is left out (its bot stopped). Live only: paper bots never use it (their
    // positions are not real, and an A/B pair would count each other). group "" = "live". The Python bots
    // share the same files.
    bool enabled = false;
    std::string asset_class;  // crypto | stocks | metals
    double weight = 1.0;
    double limit_usd = 1500.0;
    double factor = 0.3;
    double shift_bp = 0.0;
    std::string dir = "~/.naive_mm/portfolio";
    std::string group;
    double stale_s = 30.0;
    double interval_ms = 100.0;  // how often each bot reads the others' files (its own: on a change, and each second)
};

struct AddGuardConfig {
    // direction = "both": after the futures mid moved more than `drawdown` against a side within
    // `window_s`, an order on that side may take the position there to `limit_share` of
    // max_position_usd at most, for `pause_s` past the last such price. drawdown 0 = off.
    double window_s = 300.0;
    double drawdown = 0.0;
    double pause_s = 60.0;
    double limit_share = 0.5;
};

struct VolMarkupConfig {
    // The markup scaled by volatility: the reference mid (Binance futures, else the spot book) sampled each
    // second, its squared returns in an exponential mean over short_s (now) and one over long_s (the usual);
    // every order's markup x sqrt(short / long), within low..high (x1 for warmup_s after start). 19 Oct, 6 h at
    // 60 s / 30 min: mean x1.0, median x0.87-0.94, p90 x1.45, at x0.8 31-41% of the time, at x2 1-2%. Off unless enabled.
    bool enabled = false;
    double short_s = 60.0;
    double long_s = 1800.0;
    double low = 0.8;
    double high = 2.0;
    double warmup_s = 300.0;
    // The factor is (short / long) ^ power: 0.5 (the default) the ratio of the volatilities themselves;
    // 0.75 a volatility 1.5x the usual gives x1.84 (0.5: x1.5), 0.67x gives x0.55 (then low).
    double power = 0.5;
};

struct DriftConfig {
    // The markup of a side times `markup_factor` while the futures mid (Binance spot with [futures]
    // off) moved against it over the last `window_s` by `threshold` or more; held `hold_s` past the
    // last such reading. The move is logged on every placement and fill whatever the factor.
    double window_s = 10.0;
    double threshold = 0.00002;  // 0.2 bp
    double hold_s = 2.0;
    double markup_factor = 1.0;
};

struct TrendConfig {
    // No opening order right after a sharp move against it: while the futures mid is more than
    // `drawdown` under its highest of the last `window_s`, a long bot places no buy (a short bot,
    // the same for sells over the lowest); the pause runs `pause_s` past the last price that far.
    // drawdown 0 = off.
    double window_s = 120.0;
    double drawdown = 0.003;  // 30 bp (BTC)
    double pause_s = 60.0;
};

struct LagConfig {
    // Market data read late: when a Binance futures ticker or a Binance spot trade is read `max_ms`
    // or more after its exchange's own time for it (the host clock set against the exchange's by the
    // offset measured to Arcus), or the event loop runs that late, both orders are cancelled and
    // nothing is placed until `pause_ms` after the last such reading. 0 = off.
    double max_ms = 200.0;
    double pause_ms = 500.0;
    // A Bybit ticker read late pauses the same way only from this lag (ms, at least max_ms); 0: never.
    double bybit_max_ms = 0.0;
    // A stream that stays late: a pause that starts under `calm_ms` after the last one ended runs
    // twice as long as that one, up to `max_pause_ms`; on-time data for `calm_ms` brings it back to
    // `pause_ms`. max_pause_ms 0: every pause is pause_ms.
    double max_pause_ms = 0.0;
    double calm_ms = 60'000.0;
};

struct ImbalanceConfig {
    // Lopsided Arcus book: when one side has more than `ratio` times the volume of the other on the
    // first `levels` levels, the markup of an order on the thin side is multiplied by `factor`, and
    // stays so for `hold_ms`. ratio 0 = off. `sides`: "open" or "both" ("buy": the old name of "open").
    int levels = 5;
    double ratio = 5.0;
    double factor = 5.0;
    double hold_ms = 300.0;
    std::string sides = "open";
};

struct AgentConfig {
    int depth = 10;  // levels per side exposed to the strategy
    double status_interval_s = 5.0;
    // The health line, in the console and the orders log this often; 0 = off
    double health_interval_s = 60.0;
    // A line in the orders log on every change of a book's best bid price; false = none
    bool tob_lines = true;
    // ... at most one a book this often (ms); 0 = every change. The first change after a quiet spell is
    // written at once, so when a move started stays exact; the changes within it are not written.
    double tob_line_ms = 250.0;
    // The Python bot's garbage collector tuning: read and logged, nothing to tune in C++.
    bool gc_tune = true;
    // The buy/sell price lines: at most one this often, ms; 0 = one on every change of a price.
    double quote_line_ms = 0.0;
    // A stream on several connections (feeds with connections > 1; live, the Arcus trades on main and on the
    // cancel lanes; paper, its two trades connections): each copy is timed against the first copy of the same
    // update, and a connection whose copies come more than this far behind at the median over 2 min is
    // reconnected (at most every 2 min each). A server can hand out a connection that stays late: 19 Oct, after
    // a reconnect, one to the same Cloudflare address had every Arcus trade 73-78 ms late for an hour. 0 = never.
    double slow_link_ms = 30.0;  // 30 from 19 Oct evening (was 20: paper and live reconnected links only 21-24 ms behind)
};

struct Config {
    BinanceConfig binance;
    ArcusConfig arcus;
    PricingConfig pricing;
    MlConfig ml;
    TradingConfig trading;
    GuardConfig guard;
    FuturesConfig futures;
    TradesConfig trades;
    BybitConfig bybit;
    OkxConfig okx;
    CTraderConfig ctrader;
    ArcusGuardConfig arcus_guard;
    TakesConfig takes;
    TrendConfig trend;
    AddGuardConfig add_guard;
    OpenGuardConfig open_guard;
    OpenHaltConfig open_halt;
    PortfolioConfig portfolio;
    VolMarkupConfig vol_markup;
    DriftConfig drift;
    LagConfig lag;
    ImbalanceConfig imbalance;
    AgentConfig agent;
    std::string log_level = "INFO";

    // Throws std::invalid_argument with what is wrong.
    void validate() const;

private:
    void validate_ml() const;
};

inline const std::vector<std::string> DOLLAR_QUOTES {"USDT", "USDC", "FDUSD", "USD"};
constexpr double MAX_FRACTION = 0.01;  // 100 bp
constexpr double MAX_TREND = 0.05;  // 500 bp: a fall over minutes can be more than 1%

// The settings in effect, one line per section: defaults merged with the file and
// the command line. Credentials live elsewhere and never show.
std::vector<std::string> describe(const Config& config);

// The defaults, then the file, then each "section.key=value" of `overrides`
// (--set). Throws std::invalid_argument on anything wrong (and std::runtime_error
// when the file cannot be read).
Config load_config(const std::optional<std::string>& path, const std::vector<std::string>& overrides = {});

// The ML model's parameters as [ml] gives them.
ml::Params ml_params(const MlConfig& m);

}  // namespace nmm
