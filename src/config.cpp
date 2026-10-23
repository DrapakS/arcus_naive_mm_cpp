#include "config.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "lgbm.hpp"
#include "ml.hpp"
#include "portfolio.hpp"
#include "tomlplusplus/toml.hpp"
#include "trading/guard.hpp"
#include "trading/opens.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

ConfigValue ConfigValue::string(std::string s) {
    ConfigValue v;
    v.kind = Kind::String;
    v.repr = pyrepr(s);
    v.text = std::move(s);
    return v;
}

ConfigValue ConfigValue::integer(int64_t n) {
    ConfigValue v;
    v.kind = Kind::Integer;
    v.number = static_cast<double>(n);
    v.repr = std::to_string(n);
    return v;
}

ConfigValue ConfigValue::floating(double x) {
    ConfigValue v;
    v.kind = Kind::Float;
    v.number = x;
    v.repr = pyrepr(x);
    return v;
}

std::vector<std::string> strings(const ConfigList& list) {
    std::vector<std::string> out;
    for (const auto& v : list) out.push_back(v.is_string() ? v.text : v.repr);
    return out;
}

std::vector<std::pair<std::string, double>> numbers(const ConfigTable& table) {
    std::vector<std::pair<std::string, double>> out;
    for (const auto& [name, v] : table) out.emplace_back(name, v.number);
    return out;
}

namespace {

// ------------------------------------------------------------- the fields

template <class F> void fields(BinanceConfig& c, F&& f) {
    f("symbol", c.symbol); f("ws_url", c.ws_url); f("rest_url", c.rest_url); f("snapshot_limit", c.snapshot_limit);
    f("update_speed_ms", c.update_speed_ms); f("book_ticker", c.book_ticker); f("stale_timeout_s", c.stale_timeout_s);
    f("max_book_age_s", c.max_book_age_s); f("connections", c.connections);
}
template <class F> void fields(ArcusConfig& c, F&& f) {
    f("market", c.market); f("ws_url", c.ws_url); f("rest_url", c.rest_url); f("n_levels", c.n_levels);
    f("stale_timeout_s", c.stale_timeout_s); f("max_book_age_s", c.max_book_age_s); f("connections", c.connections);
    f("feed_assets", c.feed_assets);
}
template <class F> void fields(PricingConfig& c, F&& f) {
    f("model", c.model); f("reference", c.reference); f("basis_s", c.basis_s); f("window", c.window);
    f("min_samples", c.min_samples);
}
template <class F> void fields(MlConfig& c, F&& f) {
    f("weights", c.weights); f("kind", c.kind); f("lightgbm_model", c.lightgbm_model); f("features", c.features);
    f("scaled", c.scaled); f("mean", c.mean); f("scale", c.scale); f("target_window_s", c.target_window_s);
    f("target_base", c.target_base); f("baseline_tau_s", c.baseline_tau_s); f("skip_missing", c.skip_missing);
    f("required", c.required); f("target_price", c.target_price); f("max_gap_s", c.max_gap_s);
    f("arcus_max_gap_s", c.arcus_max_gap_s); f("eval_interval_ms", c.eval_interval_ms);
    f("reuse_ms", c.reuse_ms);
}
template <class F> void fields(TradingConfig& c, F&& f) {
    f("mode", c.mode); f("markup", c.markup); f("direction", c.direction); f("max_position_usd", c.max_position_usd);
    f("max_position_resume", c.max_position_resume); f("off_hours_markup_factor", c.off_hours_markup_factor);
    f("skew_open", c.skew_open); f("skew_close", c.skew_close); f("exit_after_s", c.exit_after_s);
    f("exit_loss", c.exit_loss); f("close_markup_cut", c.close_markup_cut); f("tol", c.tol); f("tol_away", c.tol_away);
    f("close_tol_away", c.close_tol_away); f("order_size", c.order_size); f("never_lead", c.never_lead);
    f("came_hold_ms", c.came_hold_ms); f("dms_timeout_s", c.dms_timeout_s); f("cancel_lanes", c.cancel_lanes);
    f("presign_cancel_s", c.presign_cancel_s); f("slow_reconnect_ms", c.slow_reconnect_ms); f("orders_log", c.orders_log);
}
template <class F> void fields(GuardConfig& c, F&& f) {
    f("window_ms", c.window_ms); f("drawdown", c.drawdown); f("pause_ms", c.pause_ms);
}
template <class F> void fields(FuturesConfig& c, F&& f) {
    f("enabled", c.enabled); f("symbol", c.symbol); f("ws_url", c.ws_url); f("stale_timeout_s", c.stale_timeout_s);
    f("connections", c.connections); f("max_book_age_s", c.max_book_age_s); f("window_ms", c.window_ms);
    f("drawdown", c.drawdown); f("pause_ms", c.pause_ms);
}
template <class F> void fields(TradesConfig& c, F&& f) {
    f("enabled", c.enabled); f("ws_url", c.ws_url); f("stale_timeout_s", c.stale_timeout_s);
    f("connections", c.connections); f("window_ms", c.window_ms); f("drawdown", c.drawdown); f("pause_ms", c.pause_ms);
}
template <class F> void fields(BybitConfig& c, F&& f) {
    f("enabled", c.enabled); f("category", c.category); f("symbol", c.symbol); f("ws_url", c.ws_url);
    f("stale_timeout_s", c.stale_timeout_s); f("connections", c.connections); f("window_ms", c.window_ms);
    f("drawdown", c.drawdown); f("pause_ms", c.pause_ms);
}
template <class F> void fields(OkxConfig& c, F&& f) {
    f("enabled", c.enabled); f("inst_id", c.inst_id); f("ws_url", c.ws_url); f("stale_timeout_s", c.stale_timeout_s);
    f("connections", c.connections); f("window_ms", c.window_ms); f("drawdown", c.drawdown); f("pause_ms", c.pause_ms);
}
template <class F> void fields(CTraderConfig& c, F&& f) {
    f("enabled", c.enabled); f("symbol", c.symbol); f("credentials", c.credentials); f("heartbeat_s", c.heartbeat_s);
    f("window_ms", c.window_ms); f("drawdown", c.drawdown); f("pause_ms", c.pause_ms);
}
template <class F> void fields(QueueConfig& c, F&& f) {
    f("snap", c.snap); f("step_share", c.step_share); f("wall_x", c.wall_x); f("paper_queue", c.paper_queue);
}
template <class F> void fields(TakesConfig& c, F&& f) {
    f("min_usd", c.min_usd); f("pause_ms", c.pause_ms); f("min_takes", c.min_takes); f("min_move", c.min_move);
    f("score_ms", c.score_ms);
}
template <class F> void fields(ArcusGuardConfig& c, F&& f) {
    f("window_ms", c.window_ms); f("drawdown", c.drawdown); f("pause_ms", c.pause_ms);
}
template <class F> void fields(OpenGuardConfig& c, F&& f) {
    f("opens", c.opens); f("tz", c.tz); f("before_s", c.before_s); f("after_s", c.after_s); f("limit_share", c.limit_share);
}
template <class F> void fields(OpenHaltConfig& c, F&& f) {
    f("opens", c.opens); f("tz", c.tz); f("before_s", c.before_s); f("after_s", c.after_s); f("closing", c.closing);
}
template <class F> void fields(VolMarkupConfig& c, F&& f) {
    f("enabled", c.enabled); f("short_s", c.short_s); f("long_s", c.long_s); f("low", c.low); f("high", c.high);
    f("warmup_s", c.warmup_s); f("power", c.power);
}
template <class F> void fields(PortfolioConfig& c, F&& f) {
    f("enabled", c.enabled); f("asset_class", c.asset_class); f("weight", c.weight); f("limit_usd", c.limit_usd);
    f("factor", c.factor); f("shift_bp", c.shift_bp); f("dir", c.dir); f("group", c.group); f("stale_s", c.stale_s); f("interval_ms", c.interval_ms);
}
template <class F> void fields(AddGuardConfig& c, F&& f) {
    f("window_s", c.window_s); f("drawdown", c.drawdown); f("pause_s", c.pause_s); f("limit_share", c.limit_share);
}
template <class F> void fields(DriftConfig& c, F&& f) {
    f("window_s", c.window_s); f("threshold", c.threshold); f("hold_s", c.hold_s); f("markup_factor", c.markup_factor);
}
template <class F> void fields(TrendConfig& c, F&& f) {
    f("window_s", c.window_s); f("drawdown", c.drawdown); f("pause_s", c.pause_s);
}
template <class F> void fields(LagConfig& c, F&& f) {
    f("max_ms", c.max_ms); f("pause_ms", c.pause_ms); f("bybit_max_ms", c.bybit_max_ms);
    f("max_pause_ms", c.max_pause_ms); f("calm_ms", c.calm_ms);
}
template <class F> void fields(ImbalanceConfig& c, F&& f) {
    f("levels", c.levels); f("ratio", c.ratio); f("factor", c.factor); f("hold_ms", c.hold_ms); f("sides", c.sides);
}
template <class F> void fields(AgentConfig& c, F&& f) {
    f("depth", c.depth); f("status_interval_s", c.status_interval_s); f("health_interval_s", c.health_interval_s);
    f("tob_lines", c.tob_lines); f("tob_line_ms", c.tob_line_ms); f("gc_tune", c.gc_tune); f("quote_line_ms", c.quote_line_ms);
    f("slow_link_ms", c.slow_link_ms);
}

// Every section of the config, by name, in the order Python's dataclass lists them.
template <class F> void sections(Config& c, F&& f) {
    f("binance", c.binance); f("arcus", c.arcus); f("pricing", c.pricing); f("ml", c.ml); f("trading", c.trading);
    f("guard", c.guard); f("futures", c.futures); f("trades", c.trades); f("bybit", c.bybit);
    f("okx", c.okx); f("ctrader", c.ctrader); f("arcus_guard", c.arcus_guard); f("takes", c.takes); f("trend", c.trend); f("add_guard", c.add_guard);
    f("open_guard", c.open_guard); f("open_halt", c.open_halt); f("portfolio", c.portfolio); f("vol_markup", c.vol_markup);
    f("drift", c.drift); f("lag", c.lag); f("imbalance", c.imbalance); f("queue", c.queue); f("agent", c.agent);
}

// Keys that changed meaning: a config still using them is stopped with the new name.
const std::map<std::string, std::string>& renamed() {
    static const std::map<std::string, std::string> keys = [] {
        std::map<std::string, std::string> m;
        for (const char* section : {"guard", "futures", "arcus_guard"})
            m[std::string(section) + ".window"] = std::format(
                "use {}.window_ms, the mean is now over a time span (ms), not a number of updates", section);
        return m;
    }();
    return keys;
}

// ------------------------------------------------------------- TOML values

std::string repr(const toml::node& node);

std::string repr_table(const toml::table& t) {
    std::vector<std::string> parts;
    for (auto&& [key, value] : t) parts.push_back(pyrepr(std::string_view(key.str())) + ": " + repr(value));
    return "{" + join(parts, ", ") + "}";
}

std::string repr(const toml::node& node) {
    switch (node.type()) {
    case toml::node_type::string: return pyrepr(std::string_view(*node.value<std::string_view>()));
    case toml::node_type::integer: return std::to_string(*node.value<int64_t>());
    case toml::node_type::floating_point: return pyrepr(*node.value<double>());
    case toml::node_type::boolean: return *node.value<bool>() ? "True" : "False";
    case toml::node_type::array: {
        std::vector<std::string> parts;
        for (auto&& item : *node.as_array()) parts.push_back(repr(item));
        return "[" + join(parts, ", ") + "]";
    }
    case toml::node_type::table: return repr_table(*node.as_table());
    default: {
        std::ostringstream out;
        node.visit([&out](auto&& v) { out << v; });
        return out.str();
    }
    }
}

ConfigValue value_of(const toml::node& node) {
    switch (node.type()) {
    case toml::node_type::string: return ConfigValue::string(std::string(*node.value<std::string_view>()));
    case toml::node_type::integer: return ConfigValue::integer(*node.value<int64_t>());
    case toml::node_type::floating_point: return ConfigValue::floating(*node.value<double>());
    case toml::node_type::boolean: {
        ConfigValue v;
        v.kind = ConfigValue::Kind::Bool;
        v.number = *node.value<bool>() ? 1.0 : 0.0;
        v.repr = *node.value<bool>() ? "True" : "False";
        return v;
    }
    default: {
        ConfigValue v;
        v.repr = repr(node);
        return v;
    }
    }
}

// A table's entries in the order the file wrote them (toml++ keeps them sorted by key).
std::vector<std::pair<std::string, const toml::node*>> in_file_order(const toml::table& t) {
    std::vector<std::tuple<toml::source_position, std::string, const toml::node*>> entries;
    for (auto&& [key, value] : t) entries.emplace_back(value.source().begin, std::string(key.str()), &value);
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        const auto& pa = std::get<0>(a);
        const auto& pb = std::get<0>(b);
        return std::tie(pa.line, pa.column) < std::tie(pb.line, pb.column);
    });
    std::vector<std::pair<std::string, const toml::node*>> out;
    for (auto& [pos, key, node] : entries) out.emplace_back(key, node);
    return out;
}

[[noreturn]] void wrong_type(const std::string& key, const char* expected, const toml::node& node) {
    throw std::invalid_argument(std::format("config key {} must be {}, got {}", key, expected, repr(node)));
}

void coerce(double& out, const toml::node& node, const std::string& key) {
    if (node.is_integer()) {
        out = static_cast<double>(*node.value<int64_t>());
    } else if (node.is_floating_point()) {
        double x = *node.value<double>();
        if (!std::isfinite(x)) throw std::invalid_argument(std::format("config key {} must be a finite number, got {}", key, repr(node)));
        out = x;
    } else {
        wrong_type(key, "float", node);
    }
}

void coerce(int& out, const toml::node& node, const std::string& key) {
    if (!node.is_integer()) wrong_type(key, "int", node);
    int64_t n = *node.value<int64_t>();
    if (n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max()) wrong_type(key, "int within 32 bits", node);
    out = static_cast<int>(n);
}

void coerce(bool& out, const toml::node& node, const std::string& key) {
    if (!node.is_boolean()) wrong_type(key, "bool", node);
    out = *node.value<bool>();
}

void coerce(std::string& out, const toml::node& node, const std::string& key) {
    if (!node.is_string()) wrong_type(key, "str", node);
    out = std::string(*node.value<std::string_view>());
}

void coerce(ConfigList& out, const toml::node& node, const std::string& key) {
    if (!node.is_array()) wrong_type(key, "list", node);
    out.clear();
    for (auto&& item : *node.as_array()) out.push_back(value_of(item));
}

void coerce(ConfigTable& out, const toml::node& node, const std::string& key) {
    if (!node.is_table()) wrong_type(key, "dict", node);
    out.clear();
    for (auto& [name, value] : in_file_order(*node.as_table())) out.emplace_back(name, value_of(*value));
}

template <class S>
void merge_section(S& section, const toml::table& values, const std::string& prefix) {
    for (auto& [key, node] : in_file_order(values)) {
        std::string full = prefix + key;
        if (auto it = renamed().find(full); it != renamed().end())
            throw std::invalid_argument(std::format("config key {} is gone: {}", full, it->second));
        bool found = false;
        fields(section, [&](std::string_view name, auto& member) {
            if (!found && name == key) {
                found = true;
                coerce(member, *node, full);
            }
        });
        if (!found) throw std::invalid_argument("unknown config key: " + full);
    }
}

void merge(Config& config, const toml::table& values) {
    for (auto& [key, node] : in_file_order(values)) {
        if (auto it = renamed().find(key); it != renamed().end())
            throw std::invalid_argument(std::format("config key {} is gone: {}", key, it->second));
        if (key == "log_level") {
            coerce(config.log_level, *node, key);
            continue;
        }
        bool found = false;
        sections(config, [&](std::string_view name, auto& section) {
            if (found || name != key) return;
            found = true;
            if (!node->is_table()) throw std::invalid_argument(std::format("config key {} must be a table", key));
            merge_section(section, *node->as_table(), key + ".");
        });
        if (!found) throw std::invalid_argument("unknown config key: " + key);
    }
}

// "a.b=value" as the table {"a": {"b": value}}; the value is read as TOML (1.5, true,
// "text"), a bare word as a string.
toml::table override_table(const std::string& item) {
    auto eq = item.find('=');
    std::string key = eq == std::string::npos ? "" : item.substr(0, eq);
    if (eq == std::string::npos || strip(key).empty())
        throw std::invalid_argument(std::format("--set {}: expected section.key=value", pyrepr(item)));
    std::string text(strip(std::string_view(item).substr(eq + 1)));
    toml::table parsed;
    bool ok = true;
    try {
        parsed = toml::parse("v = " + text);
    } catch (const toml::parse_error&) {
        ok = false;
    }
    std::vector<std::string> path;
    size_t start = 0;
    for (;;) {
        size_t dot = key.find('.', start);
        path.emplace_back(strip(std::string_view(key).substr(start, dot == std::string::npos ? std::string::npos : dot - start)));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    toml::table root;
    toml::table* node = &root;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        node->insert_or_assign(path[i], toml::table {});
        node = (*node)[path[i]].as_table();
    }
    if (ok) node->insert_or_assign(path.back(), std::move(*parsed.get("v")));  // moved: a copy loses where each key stood (an inline table's order)
    else node->insert_or_assign(path.back(), text);
    return root;
}

// ------------------------------------------------------------- describe

std::string repr_field(const std::string& v) { return pyrepr(std::string_view(v)); }
std::string repr_field(double v) { return pyrepr(v); }
std::string repr_field(int v) { return std::to_string(v); }
std::string repr_field(bool v) { return v ? "True" : "False"; }
std::string repr_field(const ConfigList& list) {
    std::vector<std::string> parts;
    for (const auto& v : list) parts.push_back(v.repr);
    return "[" + join(parts, ", ") + "]";
}
std::string repr_field(const ConfigTable& table) {
    std::vector<std::string> parts;
    for (const auto& [name, v] : table) parts.push_back(pyrepr(std::string_view(name)) + ": " + v.repr);
    return "{" + join(parts, ", ") + "}";
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

bool isalnum_ascii(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c); });
}

std::string g(double x) { return std::format("{:g}", x); }

}  // namespace

std::vector<std::string> describe(const Config& config) {
    Config copy = config;
    std::vector<std::string> lines {"log_level=" + repr_field(copy.log_level)};
    sections(copy, [&](std::string_view name, auto& section) {
        std::vector<std::string> pairs;
        fields(section, [&](std::string_view field, auto& value) { pairs.push_back(std::format("{}={}", field, repr_field(value))); });
        lines.push_back(std::format("[{}] {}", name, join(pairs, " ")));
    });
    return lines;
}

Config load_config(const std::optional<std::string>& path, const std::vector<std::string>& overrides) {
    Config config;
    if (path) {
        toml::table file;
        try {
            file = toml::parse_file(*path);
        } catch (const toml::parse_error& err) {
            const auto& where = err.source().begin;
            throw std::invalid_argument(std::format("{}: {} (at line {}, column {})", *path, err.description(), where.line, where.column));
        }
        merge(config, file);
    }
    for (const auto& item : overrides) merge(config, override_table(item));
    config.validate();
    return config;
}

void Config::validate() const {
    // The symbols default to BTC: an ETH config that forgets one prices ETH off BTC. The asset
    // quoted in a dollar stablecoin, exactly: ETHBTC or ETHFIUSDT are other markets. Another
    // asset only when arcus.feed_assets names it (GLD-USD priced off gold).
    for (const auto& asset : arcus.feed_assets)
        if (!asset.is_string() || !isalnum_ascii(asset.text))
            throw std::invalid_argument(std::format("arcus.feed_assets: {} is not an asset name such as \"XAU\"", asset.repr));
    std::string base = upper(arcus.market.substr(0, arcus.market.find('-')));
    std::vector<std::string> assets {base};
    for (const auto& asset : arcus.feed_assets)
        if (upper(asset.text) != base) assets.push_back(upper(asset.text));
    struct Feed {
        const char* name;
        const std::string& symbol;
        bool on;
    };
    for (const Feed& feed : {Feed {"binance", binance.symbol, true}, Feed {"futures", futures.symbol, futures.enabled},
                             Feed {"bybit", bybit.symbol, bybit.enabled}}) {
        std::string symbol(remove_prefix(upper(feed.symbol), "1000"));  // 1000PEPEUSDT: a thousand PEPE
        bool matches = std::any_of(assets.begin(), assets.end(), [&](const std::string& a) {
            return starts_with(symbol, a) && contains(DOLLAR_QUOTES, symbol.substr(a.size()));
        });
        if (feed.on && !matches)
            throw std::invalid_argument(std::format(
                "{}.symbol = {} is not {} against the dollar, but arcus.market = {}: set every symbol to the same asset "
                "(another asset only when listed in arcus.feed_assets)",
                feed.name, pyrepr(std::string_view(feed.symbol)), join(assets, " or "), pyrepr(std::string_view(arcus.market))));
    }
    if (okx.enabled) {
        std::vector<std::string> parts = split(upper(okx.inst_id), '-');
        if (parts.size() != 3 || !contains(assets, parts[0]) || !contains(DOLLAR_QUOTES, parts[1]) || parts[2] != "SWAP")
            throw std::invalid_argument(std::format(
                "okx.inst_id = {} is not the {} perpetual against the dollar (\"{}-USDT-SWAP\"), but arcus.market = {}",
                pyrepr(std::string_view(okx.inst_id)), join(assets, " or "), base, pyrepr(std::string_view(arcus.market))));
    }
    // Fractions of the price: 0.00001 = 0.1 bp. One over 1% is a value meant in bp.
    const TradingConfig& t = trading;
    for (auto [key, value] : std::initializer_list<std::pair<const char*, double>> {
             {"trading.markup", t.markup}, {"trading.tol", t.tol}, {"trading.tol_away", t.tol_away},
             {"trading.close_tol_away", t.close_tol_away}, {"guard.drawdown", guard.drawdown},
             {"futures.drawdown", futures.drawdown}, {"trades.drawdown", trades.drawdown},
             {"bybit.drawdown", bybit.drawdown}, {"okx.drawdown", okx.drawdown}, {"ctrader.drawdown", ctrader.drawdown},
             {"arcus_guard.drawdown", arcus_guard.drawdown}}) {
        if (value > MAX_FRACTION)
            throw std::invalid_argument(std::format("{} = {} is {} bp: it is a fraction of the price (0.00001 = 0.1 bp), at most {}",
                                                    key, g(value), g(value * 1e4), g(MAX_FRACTION)));
    }
    for (auto [key, value] : std::initializer_list<std::pair<const char*, double>> {
             {"binance.stale_timeout_s", binance.stale_timeout_s}, {"binance.max_book_age_s", binance.max_book_age_s},
             {"arcus.stale_timeout_s", arcus.stale_timeout_s}, {"arcus.max_book_age_s", arcus.max_book_age_s},
             {"futures.stale_timeout_s", futures.stale_timeout_s}, {"futures.max_book_age_s", futures.max_book_age_s},
             {"trades.stale_timeout_s", trades.stale_timeout_s}, {"bybit.stale_timeout_s", bybit.stale_timeout_s},
             {"okx.stale_timeout_s", okx.stale_timeout_s}}) {
        if (value < 0) throw std::invalid_argument(std::format("{} must be >= 0 (0 = off)", key));
    }
    if (!(agent.status_interval_s >= 0.5)) throw std::invalid_argument("agent.status_interval_s must be at least 0.5");
    if (agent.health_interval_s && !(5 <= agent.health_interval_s && agent.health_interval_s <= 3600))
        throw std::invalid_argument("agent.health_interval_s must be 0 (off) or within 5..3600");
    if (!(0 <= agent.slow_link_ms && agent.slow_link_ms <= 10'000))
        throw std::invalid_argument("agent.slow_link_ms must be within 0..10000 (0 = never reconnect a late connection)");
    if (!(0 <= agent.tob_line_ms && agent.tob_line_ms <= 60'000))
        throw std::invalid_argument("agent.tob_line_ms must be within 0..60000 (0 = a line on every change)");
    if (!(0 <= agent.quote_line_ms && agent.quote_line_ms <= 60'000))
        throw std::invalid_argument("agent.quote_line_ms must be within 0..60000 (0 = a line on every change)");
    if (!(1 <= arcus.n_levels && arcus.n_levels <= 100)) throw std::invalid_argument("arcus.n_levels must be within 1..100");
    if (!(1 <= binance.connections && binance.connections <= 4)) throw std::invalid_argument("binance.connections must be within 1..4");
    if (!(1 <= arcus.connections && arcus.connections <= 4)) throw std::invalid_argument("arcus.connections must be within 1..4");
    if (!(1 <= binance.snapshot_limit && binance.snapshot_limit <= 5000))
        throw std::invalid_argument("binance.snapshot_limit must be within 1..5000");
    if (binance.update_speed_ms != 100 && binance.update_speed_ms != 1000)
        throw std::invalid_argument("binance.update_speed_ms must be 100 or 1000");
    const PricingConfig& p = pricing;
    if (!(1 <= p.min_samples) || (!p.basis_s && p.min_samples > p.window))
        throw std::invalid_argument("pricing needs min_samples >= 1, and <= window when basis_s is 0");
    if (p.reference != "spot" && p.reference != "futures") throw std::invalid_argument("pricing.reference must be \"spot\" or \"futures\"");
    if (p.model == "basis" && p.reference == "futures" && !futures.enabled)
        throw std::invalid_argument("pricing.reference = \"futures\" needs [futures] enabled = true");
    if (!(0 <= p.basis_s && p.basis_s <= 3600))
        throw std::invalid_argument("pricing.basis_s must be within 0..3600 (0 = the window of samples)");
    if (p.model != "basis" && p.model != "ml") throw std::invalid_argument("pricing.model must be \"basis\" or \"ml\"");
    validate_ml();
    if (t.mode != "off" && t.mode != "quote" && t.mode != "paper" && t.mode != "live")
        throw std::invalid_argument("trading.mode must be off, quote, paper or live");
    if (t.direction != "long" && t.direction != "short" && t.direction != "both")
        throw std::invalid_argument("trading.direction must be \"long\", \"short\" or \"both\"");
    if (!(t.max_position_usd > 0)) throw std::invalid_argument("trading.max_position_usd must be > 0 (dollars)");
    if (!(t.off_hours_markup_factor > 0))
        throw std::invalid_argument("trading.off_hours_markup_factor must be > 0 (2 = twice the markup off hours)");
    if (!(0 < t.max_position_resume && t.max_position_resume <= 1))
        throw std::invalid_argument("trading.max_position_resume must be within (0, 1]: a share of max_position_usd");
    if (!(0 <= t.markup && t.markup < 1) || t.tol <= 0 || t.tol_away < 0 || t.close_tol_away < 0 || t.order_size <= 0)
        throw std::invalid_argument(
            "trading needs 0 <= markup < 1, tol > 0, tol_away >= 0, close_tol_away >= 0 and order_size > 0");
    if (!(0 <= t.came_hold_ms && t.came_hold_ms <= 10'000))
        throw std::invalid_argument("trading.came_hold_ms must be within 0..10000 (0 = off)");
    if (!(0 <= t.close_markup_cut && t.close_markup_cut <= 1))
        throw std::invalid_argument("trading.close_markup_cut must be within 0..1 (0.3 = the closing markup 30% lower)");
    if (t.dms_timeout_s && !(5 <= t.dms_timeout_s && t.dms_timeout_s <= 300))
        throw std::invalid_argument("trading.dms_timeout_s must be 0 or within 5..300");
    if (!(0 <= t.cancel_lanes && t.cancel_lanes <= 4)) throw std::invalid_argument("trading.cancel_lanes must be within 0..4");
    if (t.slow_reconnect_ms < 0)
        throw std::invalid_argument("trading.slow_reconnect_ms must be >= 0 (0 = never reconnect a slow connection)");
    if (!(0 <= t.presign_cancel_s && t.presign_cancel_s <= 60))
        throw std::invalid_argument("trading.presign_cancel_s must be within 0..60");
    auto guard_ok = [](double drawdown, double window_ms, double pause_ms) {
        return !((drawdown && !(0 < window_ms && window_ms <= 600'000)) || !(0 <= drawdown && drawdown < 1) ||
                 !(0 <= pause_ms && pause_ms <= 60'000));
    };
    if (!guard_ok(guard.drawdown, guard.window_ms, guard.pause_ms))
        throw std::invalid_argument("guard needs 0 < window_ms <= 600000, 0 <= drawdown < 1, pause_ms within 0..60000");
    if (!(1 <= futures.connections && futures.connections <= 4)) throw std::invalid_argument("futures.connections must be within 1..4");
    if (!guard_ok(futures.drawdown, futures.window_ms, futures.pause_ms))
        throw std::invalid_argument("futures needs 0 < window_ms <= 600000, 0 <= drawdown < 1, pause_ms within 0..60000");
    if (!(1 <= trades.connections && trades.connections <= 4)) throw std::invalid_argument("trades.connections must be within 1..4");
    if (!guard_ok(trades.drawdown, trades.window_ms, trades.pause_ms))
        throw std::invalid_argument("trades needs 0 < window_ms <= 600000, 0 <= drawdown < 1, pause_ms within 0..60000");
    if (bybit.category != "linear" && bybit.category != "spot") throw std::invalid_argument("bybit.category must be \"linear\" or \"spot\"");
    if (!(1 <= bybit.connections && bybit.connections <= 4)) throw std::invalid_argument("bybit.connections must be within 1..4");
    if (!guard_ok(bybit.drawdown, bybit.window_ms, bybit.pause_ms))
        throw std::invalid_argument("bybit needs 0 < window_ms <= 600000, 0 <= drawdown < 1, pause_ms within 0..60000");
    if (!(1 <= okx.connections && okx.connections <= 4)) throw std::invalid_argument("okx.connections must be within 1..4");
    if (!guard_ok(okx.drawdown, okx.window_ms, okx.pause_ms))
        throw std::invalid_argument("okx needs 0 < window_ms <= 600000, 0 <= drawdown < 1, pause_ms within 0..60000");
    if (!guard_ok(ctrader.drawdown, ctrader.window_ms, ctrader.pause_ms))
        throw std::invalid_argument("ctrader needs 0 < window_ms <= 600000, 0 <= drawdown < 1, pause_ms within 0..60000");
    if (ctrader.enabled && strip(ctrader.symbol).empty())
        throw std::invalid_argument("ctrader.symbol is empty: the broker's name of the symbol (US500, USTEC, XAUUSD)");
    if (ctrader.enabled && ctrader.credentials.empty())
        throw std::invalid_argument("ctrader.credentials is empty: the TOML file of this bot's cTrader FIX login");
    if (!(0 <= queue.step_share && queue.step_share <= 1)) throw std::invalid_argument("queue.step_share must be within 0..1 (0 = off)");
    if (!(1 <= queue.wall_x && queue.wall_x <= 1000)) throw std::invalid_argument("queue.wall_x must be within 1..1000 (times order_size)");
    if (!(5 <= ctrader.heartbeat_s && ctrader.heartbeat_s <= 120)) throw std::invalid_argument("ctrader.heartbeat_s must be within 5..120");
    if (!guard_ok(arcus_guard.drawdown, arcus_guard.window_ms, arcus_guard.pause_ms))
        throw std::invalid_argument("arcus_guard needs 0 < window_ms <= 600000, 0 <= drawdown < 1, pause_ms within 0..60000");
    const TakesConfig& k = takes;
    if (k.min_usd < 0 || !(0 <= k.pause_ms && k.pause_ms <= 60'000))
        throw std::invalid_argument("takes needs min_usd >= 0 and pause_ms within 0..60000 (0 = off)");
    if (!(0 <= k.min_takes && k.min_takes <= TAKER_HISTORY) || !(0 <= k.min_move && k.min_move <= MAX_FRACTION) ||
        !(50 <= k.score_ms && k.score_ms <= 60'000))
        throw std::invalid_argument(std::format(
            "takes needs min_takes within 0..{} (0 = off), min_move within 0..{} (a fraction: 0.00002 = 0.2 bp) and score_ms "
            "within 50..60000",
            TAKER_HISTORY, g(MAX_FRACTION)));
    const TrendConfig& tr = trend;
    if (tr.drawdown > MAX_TREND)
        throw std::invalid_argument(std::format("trend.drawdown = {} is {} bp: it is a fraction of the price (0.003 = 30 bp), at most {}",
                                                g(tr.drawdown), g(tr.drawdown * 1e4), g(MAX_TREND)));
    if (tr.drawdown < 0 || (tr.drawdown && !(0 < tr.window_s && tr.window_s <= 3600)) || !(0 <= tr.pause_s && tr.pause_s <= 3600))
        throw std::invalid_argument("trend needs drawdown >= 0 (0 = off), 0 < window_s <= 3600 and pause_s within 0..3600");
    const AddGuardConfig& ad = add_guard;
    if (ad.drawdown < 0 || ad.drawdown >= 0.1 || (ad.drawdown && !(0 < ad.window_s && ad.window_s <= 3600)) ||
        !(0 <= ad.pause_s && ad.pause_s <= 3600) || !(0 < ad.limit_share && ad.limit_share <= 1))
        throw std::invalid_argument(
            "add_guard needs 0 <= drawdown < 0.1 (a fraction, 0 = off), 0 < window_s <= 3600, pause_s within 0..3600 and "
            "0 < limit_share <= 1");
    const DriftConfig& dr = drift;
    if (!(0 < dr.window_s && dr.window_s <= 600) || !(0 < dr.threshold && dr.threshold < 0.01) ||
        !(0 <= dr.hold_s && dr.hold_s <= 600) || !(1 <= dr.markup_factor && dr.markup_factor <= 10))
        throw std::invalid_argument(
            "drift needs 0 < window_s <= 600, 0 < threshold < 0.01 (a fraction: 0.00002 = 0.2 bp), 0 <= hold_s <= 600 and "
            "markup_factor within 1..10 (1 = off)");
    const OpenGuardConfig& og = open_guard;
    const OpenHaltConfig& oh = open_halt;
    for (auto [name, opens, tz] : {std::tuple<const char*, const ConfigList*, const std::string*> {"open_guard", &og.opens, &og.tz},
                                   {"open_halt", &oh.opens, &oh.tz}}) {
        if (opens->empty()) continue;
        for (const auto& item : *opens) Open::parse(item.is_string() ? item.text : item.repr);  // not a string: refused by its parse
        if (!known_time_zone(*tz))
            throw std::invalid_argument(std::format("{}.tz {} is no time zone this host knows", name, pyrepr(std::string_view(*tz))));
    }
    if (!(0 <= og.before_s && og.before_s <= 3600) || !(0 <= og.after_s && og.after_s <= 4 * 3600) ||
        !(0 <= og.limit_share && og.limit_share < 1))
        throw std::invalid_argument("open_guard needs before_s within 0..3600, after_s within 0..14400 and 0 <= limit_share < 1");
    if (!(0 <= oh.before_s && oh.before_s <= 3600) || !(0 <= oh.after_s && oh.after_s <= 3600))
        throw std::invalid_argument("open_halt needs before_s and after_s within 0..3600");
    const VolMarkupConfig& vm = vol_markup;
    if (vm.enabled && (!(1 <= vm.short_s && vm.short_s < vm.long_s && vm.long_s <= 86'400) ||
                       !(0 < vm.low && vm.low <= 1 && 1 <= vm.high && vm.high <= 10) || !(0 <= vm.warmup_s && vm.warmup_s <= 86'400) || !(0.1 <= vm.power && vm.power <= 2)))
        throw std::invalid_argument("vol_markup needs 1 <= short_s < long_s <= 86400, 0 < low <= 1 <= high <= 10, "
                                    "warmup_s within 0..86400 and power within 0.1..2");
    const PortfolioConfig& pf = portfolio;
    if (pf.enabled) {
        if (!contains(ASSET_CLASSES, pf.asset_class))
            throw std::invalid_argument("portfolio.asset_class must be one of " + join(ASSET_CLASSES, ", "));
        if (!(0 < pf.weight && pf.weight <= 10) || pf.limit_usd <= 0 || !(0 <= pf.factor && pf.factor < 1) ||
            !(0 <= pf.shift_bp && pf.shift_bp <= 5))
            throw std::invalid_argument("portfolio needs 0 < weight <= 10, limit_usd > 0, 0 <= factor < 1 and shift_bp within 0..5");
        if (!(50 <= pf.interval_ms && pf.interval_ms <= 10'000) || pf.stale_s < 3 * std::max(1.0, pf.interval_ms / 1000))
            throw std::invalid_argument("portfolio needs interval_ms within 50..10000 and stale_s of 3 s (3 heartbeats) at least");
    }
    if (ad.drawdown && t.direction != "both")
        throw std::invalid_argument("add_guard works with direction = \"both\" only (the opening side is fixed otherwise)");
    if (t.skew_open < 0 || !(0 <= t.skew_close && t.skew_close <= 1) || t.exit_after_s < 0 || !(0 <= t.exit_loss && t.exit_loss < 0.1))
        throw std::invalid_argument(
            "trading needs skew_open >= 0, skew_close within 0..1, exit_after_s >= 0 and 0 <= exit_loss < 0.1 (a fraction: "
            "0.0005 = 5 bp)");
    const LagConfig& lg = lag;
    if ((lg.max_ms && !(20 <= lg.max_ms && lg.max_ms <= 60'000)) || lg.max_ms < 0 || !(0 <= lg.pause_ms && lg.pause_ms <= 600'000))
        throw std::invalid_argument("lag needs max_ms 0 (off) or within 20..60000, and pause_ms within 0..600000");
    if (lg.bybit_max_ms && !(lg.max_ms <= lg.bybit_max_ms && lg.bybit_max_ms <= 60'000))
        throw std::invalid_argument("lag.bybit_max_ms needs 0 (Bybit never pauses) or max_ms..60000");
    if ((lg.max_pause_ms && !(lg.pause_ms <= lg.max_pause_ms && lg.max_pause_ms <= 600'000)) || lg.max_pause_ms < 0
        || !(0 <= lg.calm_ms && lg.calm_ms <= 3'600'000))
        throw std::invalid_argument("lag needs max_pause_ms 0 (pauses never grow) or pause_ms..600000, and calm_ms within 0..3600000");
    const ImbalanceConfig& imb = imbalance;
    if ((imb.ratio && !(1 <= imb.levels && imb.levels <= agent.depth)) || (imb.ratio && imb.ratio <= 1) || imb.ratio < 0 || imb.factor < 1)
        throw std::invalid_argument("imbalance needs 1 <= levels <= agent.depth, ratio 0 (off) or > 1, factor >= 1");
    if (!(0 <= imb.hold_ms && imb.hold_ms <= 60'000)) throw std::invalid_argument("imbalance needs hold_ms within 0..60000");
    if (imb.sides != "open" && imb.sides != "both" && imb.sides != "buy")
        throw std::invalid_argument("imbalance.sides must be \"open\" or \"both\" (\"buy\" is the old name of \"open\")");
    if (agent.depth < 1) throw std::invalid_argument("agent.depth must be >= 1");
    if (agent.depth > std::min(arcus.n_levels, binance.snapshot_limit))
        throw std::invalid_argument("agent.depth exceeds the depth requested from a venue");
    if (arcus.n_levels < 2 * agent.depth)
        // The Arcus book is rebuilt when fewer than agent.depth known levels are left on a side:
        // with no more than that to start with, nearly every cancel of a level would rebuild it.
        throw std::invalid_argument("arcus.n_levels must be at least twice agent.depth");
}

void Config::validate_ml() const {
    // [ml]: checked whichever model prices, so a mistyped weight shows before the switch to it.
    // The LightGBM model file itself is read only with pricing.model = "ml".
    const MlConfig& m = ml;
    if (!contains(ml::KINDS, m.kind))
        throw std::invalid_argument("ml.kind must be \"linear\" (the weights of [ml.weights]) or \"lightgbm\" (ml.lightgbm_model)");
    bool lightgbm = m.kind == "lightgbm";
    std::vector<ml::Feature> features;
    for (const auto& [name, weight] : m.weights) {
        if (!weight.is_number() || !std::isfinite(weight.number))
            throw std::invalid_argument(std::format("ml.weights.{} must be a finite number, got {}", name, weight.repr));
        if (name == ml::INTERCEPT) continue;
        auto feature = ml::parse_feature(name);
        if (!feature)
            throw std::invalid_argument(std::format("ml.weights.{}: not a feature of the notebook ({}; or {})", name, ml::FEATURE_FORMS, ml::INTERCEPT));
        features.push_back(*feature);
    }
    if (lightgbm && !m.weights.empty())
        throw std::invalid_argument(
            "[ml.weights] is the linear model: with ml.kind = \"lightgbm\" the model is ml.lightgbm_model (drop the weights, "
            "or set kind = \"linear\")");
    if (!lightgbm && (!m.features.empty() || !m.lightgbm_model.empty()))
        throw std::invalid_argument("ml.features and ml.lightgbm_model are for ml.kind = \"lightgbm\"");
    if (lightgbm) {
        std::set<std::string> seen;
        for (const auto& name : m.features) {
            if (!name.is_string() || !ml::parse_feature(name.text))
                throw std::invalid_argument(std::format("ml.features: {} is not a feature of the notebook ({})", name.repr, ml::FEATURE_FORMS));
            seen.insert(name.text);
        }
        if (seen.size() != m.features.size()) throw std::invalid_argument("ml.features lists a feature twice");
        features.clear();
        for (const auto& name : m.features) features.push_back(*ml::parse_feature(name.text));
    }
    std::set<std::string> names;
    for (const auto& f : features) names.insert(f.name);
    std::string listed = lightgbm ? "ml.features" : "[ml.weights]";
    bool bad_required = false;
    std::set<std::string> required;
    for (const auto& r : m.required) {
        if (!r.is_string()) bad_required = true;
        required.insert(r.is_string() ? r.text : r.repr);
    }
    std::vector<std::string> not_there;
    for (const auto& r : required)
        if (!names.count(r)) not_there.push_back(r);
    if (bad_required || !not_there.empty())
        throw std::invalid_argument(std::format("ml.required: every name must be a feature of {}{}", listed,
                                                names.empty() ? "" : ": not there " + join(not_there, ", ")));
    for (auto [label, table] : {std::pair<const char*, const ConfigTable*> {"mean", &m.mean}, {"scale", &m.scale}}) {
        std::set<std::string> keys;
        for (const auto& [name, value] : *table) {
            if (!value.is_number() || !std::isfinite(value.number))
                throw std::invalid_argument(std::format("ml.{}.{} must be a finite number, got {}", label, name, value.repr));
            if (std::string_view(label) == "scale" && !(value.number > 0))
                throw std::invalid_argument(std::format("ml.scale.{} must be above 0", name));
            keys.insert(name);
        }
        if (!table->empty() && (!names.empty() || !lightgbm) && keys != names) {
            std::vector<std::string> missing, extra;
            for (const auto& n : names)
                if (!keys.count(n)) missing.push_back(n);
            for (const auto& n : keys)
                if (!names.count(n)) extra.push_back(n);
            throw std::invalid_argument(std::format("ml.{} must give every feature of {} and no other{}{}", label, listed,
                                                    missing.empty() ? "" : "; missing: " + join(missing, ", "),
                                                    extra.empty() ? "" : std::format("; not {}: {}", lightgbm ? "listed" : "weighted", join(extra, ", "))));
        }
    }
    if (m.scaled && (!features.empty() || lightgbm) && !(!m.mean.empty() && !m.scale.empty()))
        throw std::invalid_argument(
            "ml.scaled = true: give the scaler the model was fit with, [ml.mean] (StandardScaler mean_) and [ml.scale] (its "
            "scale_), a value per feature");
    if ((!m.mean.empty() || !m.scale.empty()) && !m.scaled)
        throw std::invalid_argument("[ml.mean] / [ml.scale] are given: set ml.scaled = true (or drop them)");
    if (!contains(ml::PRICES, m.target_price))
        throw std::invalid_argument("ml.target_price must be one of " + join(ml::PRICES, ", "));
    if (!contains(ml::TARGET_BASES, m.target_base))
        throw std::invalid_argument("ml.target_base must be one of " + join(ml::TARGET_BASES, ", "));
    if (!(0 < m.baseline_tau_s && m.baseline_tau_s <= ml::MAX_WINDOW_S))
        throw std::invalid_argument(std::format("ml.baseline_tau_s must be within 0..{} (and above 0)", g(ml::MAX_WINDOW_S)));
    if (!(0 < m.target_window_s && m.target_window_s <= ml::MAX_WINDOW_S))
        throw std::invalid_argument(std::format("ml.target_window_s must be within 0..{} (and above 0)", g(ml::MAX_WINDOW_S)));
    if (!(m.max_gap_s > 0) || !(m.arcus_max_gap_s > 0))
        throw std::invalid_argument("ml.max_gap_s and ml.arcus_max_gap_s must be above 0");
    if (!(0 <= m.eval_interval_ms && m.eval_interval_ms <= 10'000))
        throw std::invalid_argument("ml.eval_interval_ms must be within 0..10000 (0 = only on market data)");
    if (!(0 <= m.reuse_ms && m.reuse_ms <= 100))
        throw std::invalid_argument("ml.reuse_ms must be within 0..100 (0 = a prediction on every ask)");
    if (pricing.model != "ml") return;
    if (lightgbm) {
        if (m.lightgbm_model.empty())
            throw std::invalid_argument(
                "ml.kind = \"lightgbm\" needs ml.lightgbm_model: the model file (LightGBM text format, or a pickle of a Booster "
                "or an LGBMRegressor)");
        try {
            ml::LightGBMPricer pricer(lgbm::load_model(m.lightgbm_model), strings(m.features), ml_params(m));
            features = pricer.features;
        } catch (const std::exception& exc) {
            throw std::invalid_argument(std::format("ml.lightgbm_model {}: {}", m.lightgbm_model, exc.what()));
        }
    } else if (features.empty()) {
        throw std::invalid_argument("pricing.model = \"ml\" needs the model: [ml.weights] with feature = weight lines");
    }
    for (auto [venue, section, on] : {std::tuple<const char*, const char*, bool> {"binance_futures", "futures", futures.enabled},
                                      {"bybit", "bybit", bybit.enabled}})
        if (!on && std::any_of(features.begin(), features.end(), [&](const ml::Feature& f) { return f.venue == venue; }))
            throw std::invalid_argument(std::format("the ML model reads {}_* features: set [{}] enabled = true", venue, section));
    if ((m.target_base == "futures" || m.target_base == "basis") && !futures.enabled)
        throw std::invalid_argument(std::format("ml.target_base = \"{}\" prices off Binance futures: set [futures] enabled = true", m.target_base));
}

ml::Params ml_params(const MlConfig& m) {
    ml::Params p;
    p.target_window_s = m.target_window_s;
    p.target_price = m.target_price;
    p.target_base = m.target_base;
    p.max_gap_s = m.max_gap_s;
    p.arcus_max_gap_s = m.arcus_max_gap_s;
    p.mean = numbers(m.mean);
    p.scale = numbers(m.scale);
    p.baseline_tau_s = m.baseline_tau_s;
    p.skip_missing = m.skip_missing;
    p.required = strings(m.required);
    return p;
}

}  // namespace nmm
