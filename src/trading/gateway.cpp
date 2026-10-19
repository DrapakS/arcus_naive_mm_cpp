#include "trading/gateway.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <random>

#include "net/net.hpp"
#include "trading/signing.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

double ms_since(double t) { return (monotonic() - t) * 1000.0; }

std::string random_hex(size_t n) {
    static std::mt19937_64 rng {std::random_device {}()};
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) out += digits[rng() & 15];
    return out;
}

Decimal dec(const Json& v) { return Decimal::parse(to_str(v)); }

// A field as Python's `str(x)` of `data.get(key)` would show it.
std::string field(const Json& data, std::string_view key) { return to_str(jget(data, key)); }

// A truthy string field (an optional price, a reason).
std::optional<std::string> text_field(const Json& data, std::string_view key) {
    const Json* v = jget(data, key);
    if (!truthy(v)) return std::nullopt;
    return to_str(*v);
}

bool has_key(const Json& v, std::string_view key) { return jget(v, key) != nullptr; }

int64_t int_or(const Json& data, std::string_view key, int64_t fallback) {
    const Json* v = jget(data, key);
    return v == nullptr ? fallback : to_int(*v);
}

// Cumulative filled size of an order record.
std::optional<Decimal> filled_of(const Json& data) {
    const Json* filled = jget(data, "filledSize");
    if (filled != nullptr && !filled->is_null()) return dec(*filled);
    const Json* original = jget(data, "originalSize");
    const Json* remaining = jget(data, "remainingSize");
    if (original != nullptr && !original->is_null() && remaining != nullptr && !remaining->is_null())
        return dec(*original) - dec(*remaining);
    return std::nullopt;
}

// Python's repr() of a JSON value: None, 'text', 1.5.
std::string py_repr(const Json* v) {
    if (v == nullptr || v->is_null()) return "None";
    if (v->is_string()) return pyrepr(std::string_view(v->get_string()));
    return to_str(*v);
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    size_t n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

const Json EMPTY_OBJECT = JsonObject {};

const Json& object_or_empty(const Json* v) { return truthy(v) && v->is_object() ? *v : EMPTY_OBJECT; }

template <class T>
void push_capped(std::deque<T>& q, T value, size_t cap) {
    q.push_back(std::move(value));
    while (q.size() > cap) q.pop_front();
}

}  // namespace

// ------------------------------------------------------------------ Order

const char* state_value(OrderState state) {
    switch (state) {
    case OrderState::PENDING_NEW: return "pending new";
    case OrderState::OPEN: return "open";
    case OrderState::PENDING_CANCEL: return "pending cancel";
    case OrderState::FILLED: return "filled";
    case OrderState::CANCELED: return "canceled";
    case OrderState::REJECTED: return "rejected";
    }
    return "?";
}

const char* state_name(OrderState state) {
    switch (state) {
    case OrderState::PENDING_NEW: return "PENDING_NEW";
    case OrderState::OPEN: return "OPEN";
    case OrderState::PENDING_CANCEL: return "PENDING_CANCEL";
    case OrderState::FILLED: return "FILLED";
    case OrderState::CANCELED: return "CANCELED";
    case OrderState::REJECTED: return "REJECTED";
    }
    return "?";
}

bool terminal(OrderState state) {
    return state == OrderState::FILLED || state == OrderState::CANCELED || state == OrderState::REJECTED;
}

Order::Order(std::string client_id_, std::string side_, Decimal price_, Decimal size_)
    : client_id(std::move(client_id_)), side(std::move(side_)), price(price_), size(size_), since(monotonic()),
      placed_at(monotonic()) {}

Decimal Order::trade_filled() const {
    Decimal sum;
    for (const auto& [id, size_] : trades) sum += size_;
    return max(max(sum, public_filled), user_filled);
}

bool Order::in_book() const {
    if (state != OrderState::PENDING_NEW && state != OrderState::OPEN && state != OrderState::PENDING_CANCEL) return false;
    if (book_seen) return true;
    double now = monotonic();
    if (seen_open && (!open_at || now - *open_at >= OPEN_IN_BOOK_S)) return true;
    return acked_at && now - *acked_at >= ACK_IN_BOOK_S;
}

std::string Order::str() const {
    std::string filled_text = filled ? ", filled " + fmt(filled) : "";
    return std::format("{} {} @ {} [{}{}]", side, fmt(size), fmt(price), state_value(state), filled_text);
}

// ---------------------------------------------------------------- Gateway

Gateway::Gateway(MarketSpec market_, std::function<void()> on_change_)
    : market(std::move(market_)), on_change(on_change_ ? std::move(on_change_) : [] {}), log(&get_logger("naive_mm.gateway")),
      events(&get_logger(ORDERS_LOGGER)) {
    // a mean price is shown with one decimal more than the tick has
    std::string tick = market.tick_size.str();
    auto dot = tick.find('.');
    int decimals = dot == std::string::npos ? 0 : static_cast<int>(tick.size() - dot - 1);
    int places = std::max(2, 1 + decimals);
    round_trips = std::make_unique<RoundTrips>(*events, monotonic, market.min_order_size, places);
}

OrderPtr Gateway::order() const {
    OrderPtr latest;
    for (const auto& [side, o] : orders)
        if (!latest || o->placed_at > latest->placed_at) latest = o;
    return latest;
}

void Gateway::set_order(OrderPtr o) {
    orders.clear();
    if (o) orders[o->side] = std::move(o);
}

std::vector<OrderPtr> Gateway::working() const {
    std::vector<OrderPtr> out;
    for (const char* side : {"BUY", "SELL"})
        if (auto it = orders.find(side); it != orders.end()) out.push_back(it->second);
    return out;
}

bool Gateway::is_working(const OrderPtr& o) const {
    auto it = orders.find(o->side);
    return it != orders.end() && it->second == o;
}

bool Gateway::cooling_down() const { return monotonic() < cooldown_until_; }

bool Gateway::busy() const {
    if (cooling_down()) return true;
    return std::any_of(orders.begin(), orders.end(), [](const auto& kv) {
        return kv.second->state == OrderState::PENDING_NEW || kv.second->state == OrderState::PENDING_CANCEL;
    });
}

OrderPtr Gateway::place(const std::string& side, const Decimal& price, const Decimal& size, const std::string& why,
                        std::optional<double> mid) {
    if (auto it = orders.find(side); it != orders.end()) throw std::runtime_error("already working " + it->second->str());
    auto o = std::make_shared<Order>(std::format("nmm{}{}", next_id_++, random_hex(12)), side, price, size);
    orders[side] = o;
    stats["placed"] += 1;
    send_place(o);
    events->info(">>> PLACE {} {} @ {}{}", side, fmt(size), fmt(price), why.empty() ? "" : " (" + why + ")");
    round_trips->placed(side, price, mid);
    return o;
}

void Gateway::cancel(const OrderPtr& o, const std::string& why) {
    if (o->state != OrderState::OPEN && o->state != OrderState::PENDING_NEW) return;
    set_state(*o, OrderState::PENDING_CANCEL);
    send_cancel(o, why);
}

void Gateway::set_state(Order& o, OrderState state) {
    o.state = state;
    o.since = monotonic();
    if (state == OrderState::OPEN) rejects_ = 0;  // the exchange takes our placements again
}

void Gateway::fill(const OrderPtr& o, const Decimal& filled, std::optional<std::string> price, std::optional<int64_t> seq,
                   const std::string& source) {
    Decimal delta = filled - o->filled;
    if (delta <= 0) return;
    o->filled = filled;
    rejects_ = 0;
    stats["fills"] += 1;
    Decimal before = position();
    add_position(o->side == "BUY" ? delta : -delta, seq);
    track_entry(before, price && !price->empty() ? Decimal::parse(*price) : o->price);
    events->info("<<< FILL {} {}{} (order {}/{}{}) -> position {}{}", o->side, fmt(delta),
                 price && !price->empty() ? " @ " + *price : "", fmt(filled), fmt(o->size),
                 o->order_id ? ", id " + *o->order_id : "", fmt(position()), source.empty() ? "" : ", " + source);
    if (on_fill) on_fill(o, delta);
    bool still_working = o->remaining() > 0 && !terminal(o->state);  // it may still sell the crumb
    round_trips->fill(o->side, delta, o->price, still_working);  // a maker fills at its own price
    round_trips->sync(position());  // a fill the position already had (reconnect race) is set aside
}

void Gateway::add_position(const Decimal& delta, std::optional<int64_t>) { position_ += delta; }

void Gateway::track_entry(const Decimal& before, const Decimal& price) {
    Decimal after = position();
    if (after == 0) {
        entry_price.reset();
        position_since.reset();
    } else if (before == 0 || (before > 0) != (after > 0) || !entry_price) {
        entry_price = price;  // opened, or flipped through zero
        position_since = clock();
    } else if (after.abs() > before.abs()) {  // added to: the mean of what is held
        entry_price = (*entry_price * before.abs() + price * (after.abs() - before.abs())) / after.abs();
    }
    // reduced: the entry of what is left stands
}

void Gateway::found_position(std::optional<Decimal> entry) {
    if (position() == 0) {
        entry_price.reset();
        position_since.reset();
    } else if (!entry_price) {
        entry_price = entry;
        position_since = clock();
    }
}

std::optional<std::pair<double, double>> Gateway::recently_cancelled(const std::string& side) const {
    auto it = gone_.find(side);
    if (it == gone_.end() || monotonic() - std::get<2>(it->second) >= STALE_LEVEL_S) return std::nullopt;
    return std::pair {std::get<0>(it->second).to_double(), std::get<1>(it->second).to_double()};
}

void Gateway::forget_cancelled(const std::string& side) { gone_.erase(side); }

OrderPtr Gateway::recent(const std::string& client_id) const {
    for (const auto& [id, o] : recent_)
        if (id == client_id) return o;
    return nullptr;
}

void Gateway::forget_recent(const std::string& client_id) {
    std::erase_if(recent_, [&](const auto& kv) { return kv.first == client_id; });
}

void Gateway::finish(const OrderPtr& order, OrderState state, std::optional<std::string> reason, bool fresh) {
    // A copy: `order` may be the map entry that leaving `orders` destroys below.
    const OrderPtr o = order;
    if (terminal(o->state)) return;
    if (state == OrderState::CANCELED && fresh && (!reason || *reason == "margin_canceled") && o->remaining() > 0 &&
        (o->seen_open || o->acked_at)) {
        // the orders channel says so, not a settle long after the order left the book
        gone_[o->side] = {o->price, o->remaining(), monotonic()};
    }
    set_state(*o, state);
    round_trips->settle(o->side);  // a trip left with a crumb ends with its exit order
    o->reason = reason;
    stats[state_value(state)] += 1;
    if (state == OrderState::REJECTED) {
        rejects_ += 1;
        double cooldown = std::min(REJECT_COOLDOWN_S * std::pow(2.0, std::max(rejects_ - REJECTS_BEFORE_BACKOFF, 0)), REJECT_COOLDOWN_MAX_S);
        cooldown_until_ = monotonic() + cooldown;
        call_later(cooldown, [this] { on_change(); });
        if (cooldown > REJECT_COOLDOWN_S)
            events->warning("!!! {} placements rejected in a row: not placing for {:.0f}s", rejects_, cooldown);
    }
    if (is_working(o)) orders.erase(o->side);
    forget_recent(o->client_id);
    recent_.emplace_back(o->client_id, o);
    if (recent_.size() > RECENT_ORDERS) recent_.erase(recent_.begin());
    int level = state == OrderState::REJECTED ? logging::WARNING : logging::INFO;
    std::string after = o->cancel_sent ? std::format(", {:.1f} ms after cancel sent", ms_since(*o->cancel_sent)) : "";
    if (!after.empty() && o->done_us && *o->done_us && o->cancel_sent_ns && *o->cancel_sent_ns) {
        // when the exchange says it happened, on our clock: tells a cancel that acts late from a slow channel
        double ms = static_cast<double>(*o->done_us * 1000 - *o->cancel_sent_ns) / 1e6 + clock_offset_ms.value_or(0.0);
        after += std::format(" (exchange time {:+.1f} ms, {})", ms, clock_offset_ms ? "clock offset applied" : "host clock");
    }
    events->log(level, "<<< {} {}{}{}", state_name(state), o->str(), reason ? ": " + *reason : "", after);
}

std::string Gateway::status() const {
    std::vector<std::string> parts;
    for (const auto& o : working()) parts.push_back(o->str());
    std::string working_text = parts.empty() ? "no order" : join(parts, ", ");
    std::vector<std::string> counts;
    for (const auto& [k, v] : stats) counts.push_back(std::format("{} {}", k, v));
    std::string tail = round_trips->count || round_trips->open() ? "; " + round_trips->status() : "";
    return std::format("{} {}: position {}, {}; {}{}", name, ready ? "ready" : "not ready", fmt(position()), working_text,
                       join(counts, " "), tail);
}

std::string Gateway::seen(Order& o, const std::string& source, const Decimal& filled) {
    double now = monotonic();
    auto it = std::find_if(o.seen.begin(), o.seen.end(), [&](const auto& kv) { return kv.first == source; });
    if (it == o.seen.end()) {
        o.seen.emplace_back(source, std::vector<std::pair<Decimal, double>> {});
        it = o.seen.end() - 1;
    }
    it->second.emplace_back(filled, now);
    std::vector<std::string> after;
    for (const auto& [other, marks] : o.seen) {
        if (other == source) continue;
        for (const auto& [f, at] : marks) {
            if (f >= filled) {
                after.push_back(std::format("{:.1f} ms after the {}", (now - at) * 1000, other));
                break;
            }
        }
    }
    return after.empty() ? "first" : join(after, ", ");
}

// ------------------------------------------------------------------- Conn

void Conn::attach(std::shared_ptr<WsConnection> conn) {
    ws = std::move(conn);
    next_id = 1;  // ids must increase within a connection
    connected_at = monotonic();
    replies.clear();
    events.clear();
}

std::optional<std::tuple<std::string, double, size_t>> Conn::slow(double now, double threshold_ms) const {
    for (const auto& [what, samples] : {std::pair<const char*, const std::deque<std::pair<double, double>>*> {"order events", &events},
                                        {"cancel replies", &replies}}) {
        std::vector<double> recent;
        for (const auto& [at, ms] : *samples)
            if (now - at <= SLOW_WINDOW_S) recent.push_back(ms);
        if (recent.size() >= SLOW_MIN_SAMPLES) {
            double m = median(recent);
            if (m > threshold_ms) return std::tuple {std::string(what), m, recent.size()};
        }
    }
    return std::nullopt;
}

// ----------------------------------------------------------- ArcusGateway

ArcusGateway::ArcusGateway(MarketSpec market_, ArcusCredentials credentials, Options options, std::function<void()> on_change_)
    : Gateway(std::move(market_), std::move(on_change_)), creds(std::move(credentials)), opt(std::move(options)),
      presign_cancel(opt.presign_cancel) {
    name = "arcus-trading";
    log = &get_logger("naive_mm." + name);
    opt.rest_url = std::string(rstrip(opt.rest_url, '/'));
    for (int i = 0; i < opt.cancel_lanes; ++i) lanes.push_back(std::make_unique<Conn>(std::format("lane{}", i + 1)));
}

bool ArcusGateway::can_cancel() const {
    return main_conn.ws != nullptr || std::any_of(lanes.begin(), lanes.end(), [](const auto& c) { return c->ws != nullptr; });
}

std::string ArcusGateway::status() const {
    std::string text = Gateway::status();
    if (!lanes.empty()) {
        int up = 0;
        for (const auto& c : lanes) up += c->ws != nullptr;
        text += std::format("; cancel lanes up {}/{}", up, lanes.size());
    }
    std::vector<std::string> replies;
    std::vector<const Conn*> conns {&main_conn};
    for (const auto& c : lanes) conns.push_back(c.get());
    for (const Conn* c : conns)
        if (!c->cancel_replies_ms.empty())
            replies.push_back(std::format("{} {:.0f} ms", c->name, median({c->cancel_replies_ms.begin(), c->cancel_replies_ms.end()})));
    if (!replies.empty()) text += "; cancel reply median " + join(replies, ", ");
    return text;
}

// --------------------------------------------------------------- position

Decimal ArcusGateway::position() const {
    Decimal p = snapshot_position_;
    for (const auto& [s, d] : fills_)
        if (s > snapshot_seq_) p += d;
    return p;
}

void ArcusGateway::add_position(const Decimal& delta, std::optional<int64_t> seq) {
    fills_.emplace_back(seq ? *seq : snapshot_seq_ + 1, delta);
}

void ArcusGateway::on_positions(const Json& contents) {
    // The snapshot keys the positions by marketId; a streaming update is one flat position.
    std::vector<const Json*> entries;
    const Json* positions = jget(contents, "positions");
    if ((positions == nullptr || positions->is_null()) && has_key(contents, "marketId")) {
        entries.push_back(&contents);
    } else if (positions != nullptr && positions->is_object()) {
        for (const auto& kv : positions->get_object()) entries.push_back(&kv.value());
    } else if (positions != nullptr && positions->is_array()) {
        for (const auto& e : positions->get_array()) entries.push_back(&e);
    }
    std::vector<const Json*> ours;
    for (const Json* e : entries)
        if (e->is_object() && int_or(*e, "marketId", -1) == market.market_id) ours.push_back(e);
    Decimal size;
    std::optional<Decimal> entry;
    for (const Json* e : ours) {
        // The size is signed (negative for a short) and the side says the same; frames before Oct
        // 2026 sent it unsigned. The side decides: -13.5 SHORT once read as +13.5.
        const Json* raw_size = jget(*e, "size");
        Decimal raw = truthy(raw_size) ? dec(*raw_size) : Decimal(0);
        std::string side = field(*e, "side");
        size = side == "FLAT" ? Decimal(0) : side == "SHORT" ? -raw.abs() : side == "LONG" ? raw.abs() : raw;
        try {
            entry = truthy(jget(*e, "averageEntryPrice")) ? std::optional(dec(*jget(*e, "averageEntryPrice"))) : std::nullopt;
        } catch (const std::exception&) {
        }
    }
    if (truthy(jget(contents, "isSnapshot"))) {
        int64_t seq = int_or(contents, "lastSequenceId", -1);
        snapshot_position_ = size;
        snapshot_seq_ = seq;
        snapshot_at_ = monotonic();
        std::erase_if(fills_, [seq](const auto& f) { return f.first <= seq; });
        // A position found at start, or fills missed while disconnected, are no round trip of ours.
        round_trips->sync(position(), size ? entry : std::nullopt);
        found_position(size ? entry : std::nullopt);
        position_check_.cancel();  // what streamed before the reconnect is stale
        exchange_position_.reset();
        position_warned_.reset();
    } else if (!ours.empty()) {
        // Each fill sends one; the orders channel may tell of the same fill a little later, so
        // compare only once the updates have stopped for POSITION_SETTLE_S.
        exchange_position_ = size;
        position_check_.cancel();
        position_check_ = call_later(POSITION_SETTLE_S, [this] { check_position(); });
    }
}

void ArcusGateway::check_position() {
    position_check_ = TimerHandle();
    Decimal ours = position();
    if (!exchange_position_ || *exchange_position_ == ours) {
        position_warned_.reset();
        return;
    }
    std::pair<Decimal, Decimal> key {*exchange_position_, ours};
    if (position_warned_ != key) {  // once per mismatch
        position_warned_ = key;
        events->warning("!!! exchange reports position {}, tracked {}", fmt(*exchange_position_), fmt(ours));
    }
}

// -------------------------------------------------------------- transport

namespace {
// Cancels the tasks it holds when it goes out of scope.
struct TaskGuard {
    std::vector<Task>* tasks;
    ~TaskGuard() {
        for (auto& t : *tasks) t.cancel();
    }
};
}  // namespace

awaitable<void> ArcusGateway::run() {
    for (auto& lane : lanes) lane_tasks_.push_back(Task::spawn(run_lane(lane.get()), "cancel-" + lane->name, log->name()));
    if (opt.slow_reconnect_ms) lane_tasks_.push_back(Task::spawn(watch_slow(), "slow-watch", log->name()));
    TaskGuard guard {&lane_tasks_};
    co_await run_main();
}

awaitable<void> ArcusGateway::run_main() {
    Conn& conn = main_conn;
    double backoff = 0.5;
    for (;;) {
        std::optional<double> connected_at;
        std::shared_ptr<WsConnection> ws;
        std::exception_ptr error;
        bool closed = false;
        try {
            ws = co_await WsConnection::connect(opt.ws_url, 20.0);
            connected_at = monotonic();
            conn.attach(ws);
            snapshots_.clear();
            strays_.clear();
            stray_since_.clear();
            dms_armed_ = false;
            dms_asked_.reset();  // a request lost with the old connection is not unanswered
            log->info("connected {} as {}", opt.ws_url, creds.str());
            for (const char* channel : {"orders", "positions"})
                send_frame(conn, JsonObject {{"type", "subscribe"}, {"channel", channel}, {"id", creds.address},
                                             {"accountIndex", creds.account_index}, {"market", market.symbol}});
            // public, but it shows our fills too: a third source, should the private ones lag
            send_frame(conn, JsonObject {{"type", "subscribe"}, {"channel", "trades"}, {"id", market.symbol}});
            // every fill of ours as it is matched: the first of the three to tell
            send_frame(conn, JsonObject {{"type", "subscribe"}, {"channel", "userFills"}, {"id", creds.address},
                                         {"accountIndex", creds.account_index}, {"market", market.symbol}, {"snapshot", false}});
            if (opt.dms_timeout) dms_task_ = Task::spawn(keep_dms_armed(), "dms", log->name());
            watchdog_ = Task::spawn(watch_pending(), "pending-watchdog", log->name());
            presigner_ = Task::spawn(keep_presigned(), "presign", log->name());
            FrameParser parser;
            for (;;) {
                auto text = co_await ws->read();
                if (!text) break;
                on_message(parser.parse(*text));
            }
            closed = true;
        } catch (...) {
            error = std::current_exception();
        }
        watchdog_.cancel();
        presigner_.cancel();
        bool cancelled = false;
        if (error) cancelled = co_await is_cancelled();
        if (closed) {
            backoff = 0.5;
            log->warning("trading connection closed (code={})", ws->close_code());
        } else if (!cancelled) {
            log->warning("trading connection error: {}", what(error));
        }
        dms_task_.cancel();
        ready = false;
        drop(conn);
        pull_while_blind();
        on_change();
        if (cancelled) std::rethrow_exception(error);
        if (connected_at && monotonic() - *connected_at >= RECONNECT_RESET_S)
            backoff = 0.5;  // it was up long enough: this drop is not part of a run of failures
        co_await sleep(backoff);
        backoff = std::min(backoff * 2, 30.0);
    }
}

awaitable<void> ArcusGateway::run_lane(Conn* conn) {
    double backoff = 0.5;
    for (;;) {
        std::optional<double> connected_at;
        std::shared_ptr<WsConnection> ws;
        std::exception_ptr error;
        bool closed = false;
        try {
            ws = co_await WsConnection::connect(opt.ws_url, 20.0);
            connected_at = monotonic();
            conn->attach(ws);
            log->info("cancel {} connected", conn->name);
            if (main_conn.ws == nullptr) pull_while_blind();  // main is still down: this lane is the way out
            FrameParser parser;
            for (;;) {
                auto text = co_await ws->read();
                if (!text) break;
                const Json& data = parser.parse(*text);
                if (has_key(data, "method") && has_key(data, "id")) on_response(data, *conn);
            }
            closed = true;
        } catch (...) {
            error = std::current_exception();
        }
        bool cancelled = false;
        if (error) cancelled = co_await is_cancelled();
        if (closed) {
            backoff = 0.5;
            log->warning("cancel {} closed (code={})", conn->name, ws->close_code());
        } else if (!cancelled) {
            log->warning("cancel {} error: {}", conn->name, what(error));
        }
        drop(*conn);
        if (cancelled) std::rethrow_exception(error);
        if (connected_at && monotonic() - *connected_at >= RECONNECT_RESET_S) backoff = 0.5;
        co_await sleep(backoff);
        backoff = std::min(backoff * 2, 30.0);
    }
}

awaitable<void> ArcusGateway::watch_slow() {
    for (;;) {
        co_await sleep(SLOW_CHECK_S);
        check_slow(monotonic());
    }
}

Conn* ArcusGateway::check_slow(double now) {
    std::vector<Conn*> conns {&main_conn};
    for (auto& c : lanes) conns.push_back(c.get());
    if (std::any_of(conns.begin(), conns.end(), [](Conn* c) { return c->ws == nullptr; }))
        return nullptr;  // one is reconnecting already: never two at once
    for (Conn* conn : conns) {
        if (now - conn->connected_at < SLOW_WINDOW_S || now - conn->slow_reconnect_at < SLOW_RECONNECT_GAP_S ||
            !conn->requests.empty())
            continue;
        auto slow = conn->slow(now, opt.slow_reconnect_ms);
        if (!slow) continue;
        auto [what_, median_ms, n] = *slow;
        events->warning("!!! {} connection slow: {} median {:.0f} ms over the last {:.0f} s (n {}, over {:g} ms): reconnecting",
                        conn->name, what_, median_ms, SLOW_WINDOW_S, n, opt.slow_reconnect_ms);
        conn->slow_reconnect_at = now;
        spawn(conn->ws->close_gracefully());
        return conn;
    }
    return nullptr;
}

void ArcusGateway::drop(Conn& conn) {
    conn.ws = nullptr;
    for (const auto& [id, request] : conn.requests)
        if (request->method == "cancelOrder" && request->sent)
            events->warning("!!! cancel #{} {}: no reply, connection lost {:.1f} ms after it was sent", id, conn.name,
                            ms_since(*request->sent));
    conn.requests.clear();
}

void ArcusGateway::pull_while_blind() {
    if (std::none_of(lanes.begin(), lanes.end(), [](const auto& c) { return c->ws != nullptr; })) return;
    for (const auto& o : working()) {
        if (o->state != OrderState::PENDING_CANCEL) cancel(o, "main connection lost");
        else if (!o->cancel_accepted && !cancel_in_flight(*o))
            send_cancel(o, "main connection lost", false);  // its copy on main may never have left
    }
}

void ArcusGateway::send_frame(Conn& conn, const Json& payload) {
    if (conn.ws == nullptr) throw std::runtime_error(conn.name + " connection is down");
    conn.ws->send(dump_json(payload));
}

std::vector<RequestPtr> ArcusGateway::post(const std::string& method, const Json& body, const std::string& message, int64_t ts,
                                           const OrderPtr& o, std::optional<std::string> label, bool to_lanes,
                                           std::optional<std::string> stray, std::function<std::string()> describe,
                                           std::optional<std::string> signature) {
    bool presigned = signature.has_value();
    std::string sig = signature ? *signature : creds.sign(message);
    std::vector<Conn*> conns {&main_conn};
    if (to_lanes) {
        std::vector<Conn*> up;
        if (main_conn.ws) up.push_back(&main_conn);
        for (auto& c : lanes)
            if (c->ws) up.push_back(c.get());
        if (!up.empty()) conns = up;
    }
    std::vector<std::pair<Conn*, int64_t>> copies;
    std::vector<RequestPtr> requests;
    for (Conn* conn : conns) {
        int64_t request_id = conn->next_id++;
        auto request = std::make_shared<Request>();
        request->method = label.value_or(method);
        request->order = o;
        request->stray = stray;
        request->presigned = presigned;
        request->copies = static_cast<int>(conns.size());
        if (o) {
            request->seen_open = o->seen_open;
            request->batch = o->cancels;
        }
        conn->requests[request_id] = request;
        JsonObject envelope {
            {"type", "post"},
            {"id", request_id},
            {"request", JsonObject {{"type", method}, {"payload", body}, {"apiKey", creds.api_key},
                                    {"timestamp", std::to_string(ts)}, {"signature", sig}}},
        };
        auto mark_sent = [request] {
            request->sent = monotonic();
            const OrderPtr& order = request->order;
            if (request->method == "cancelOrder" && order && !order->cancel_sent) {
                order->cancel_sent = request->sent;  // the first copy out
                order->cancel_sent_ns = time_ns();
            }
        };
        if (conn->ws == nullptr) {
            // the send fails at once; its failure is handled on the next turn, as a task's would be
            request->failed = true;
            call_soon([this, conn, request_id, request] { on_send_done(*conn, request_id, request, conn->name + " connection is down"); });
        } else {
            conn->ws->send(dump_json(envelope), [this, conn, request_id, request](const std::string& error) {
                if (!error.empty()) {
                    request->failed = true;
                    on_send_done(*conn, request_id, request, error);
                }
            });
            // Handed to the connection: on the wire at once, or right behind the frame being written
            // (as aiohttp counts a frame sent once its transport has it).
            if (!request->failed) mark_sent();
        }
        copies.emplace_back(conn, request_id);
        requests.push_back(request);
    }
    if (describe) {
        // One line once every copy is out, so logging never holds up a write.
        std::vector<std::string> states;
        for (size_t i = 0; i < copies.size(); ++i) {
            auto [conn, request_id] = copies[i];
            const RequestPtr& request = requests[i];
            states.push_back(request->sent     ? std::format("#{} {}", request_id, conn->name)
                             : request->failed ? std::format("#{} {} (failed)", request_id, conn->name)
                                               : std::format("#{} {} (queued)", request_id, conn->name));
        }
        events->info(">>> CANCEL {}; sent on {}", describe(), join(states, ", "));
    }
    return requests;
}

void ArcusGateway::on_send_done(Conn& conn, int64_t request_id, const RequestPtr& request, const std::string& error) {
    auto it = conn.requests.find(request_id);
    if (it == conn.requests.end() || it->second != request) return;
    conn.requests.erase(it);
    events->warning("!!! could not send {} #{} on {}: {}", request->method, request_id, conn.name, error);
    if (request->order && request->method == "placeOrder") {
        finish(request->order, OrderState::REJECTED, "not sent");
        on_change();
    }
}

void ArcusGateway::send_place(const OrderPtr& o) {
    int64_t ts = time_ns();
    int64_t good_til = ts / 1000 + GOOD_TIL_US;
    PlaceOrderFields f;
    f.address = creds.address;
    f.account_index = creds.account_index;
    f.market_id = market.market_id;
    f.side = o->side;
    f.price_ticks = market.price_ticks(o->price);
    f.size_quantums = market.size_quantums(o->size);
    f.time_in_force = "ALO";
    f.good_til_us = good_til;
    f.reduce_only = false;
    f.client_id = o->client_id;
    f.timestamp_ns = ts;
    JsonObject body {
        {"address", creds.address}, {"accountIndex", creds.account_index}, {"marketId", market.market_id},
        {"orderSide", o->side}, {"orderType", "LIMIT"}, {"quantity", fmt(o->size)}, {"price", fmt(o->price)},
        {"timeInForce", "ALO"}, {"goodTilTime", std::to_string(good_til)}, {"clientId", o->client_id}, {"timestamp", ts},
    };
    post("placeOrder", body, place_order_message(f), ts, o);
    presign(o);  // the placement is already on the wire
}

void ArcusGateway::presign(const OrderPtr& o) {
    if (!presign_cancel || terminal(o->state)) return;
    int64_t ts = time_ns();
    std::string message = cancel_order_message(creds.address, creds.account_index, market.market_id, ts, o->client_id);
    o->presigned = std::tuple {ts, creds.sign(message), monotonic()};
}

awaitable<void> ArcusGateway::keep_presigned() {
    for (;;) {
        co_await sleep(presign_cancel ? presign_cancel / 2 : 1.0);
        for (const auto& o : working())
            if (o->state == OrderState::OPEN || o->state == OrderState::PENDING_NEW) presign(o);
    }
}

void ArcusGateway::send_cancel(const OrderPtr& o, const std::string& why, bool keep_timing) {
    cancel_by(o->client_id, std::nullopt, o, why, keep_timing);
}

std::vector<RequestPtr> ArcusGateway::cancel_by(std::optional<std::string> client_id, std::optional<std::string> order_id,
                                                const OrderPtr& o, const std::string& why, bool keep_timing) {
    auto ready_sig = o && client_id ? o->presigned : std::nullopt;
    int64_t ts;
    std::optional<std::string> signature;
    std::string message;
    if (ready_sig && monotonic() - std::get<2>(*ready_sig) <= presign_cancel) {
        ts = std::get<0>(*ready_sig);
        signature = std::get<1>(*ready_sig);
        o->presigned.reset();
        call_soon([this, o] { presign(o); });  // one in hand for a re-cancel
    } else {
        ts = time_ns();
        message = cancel_order_message(creds.address, creds.account_index, market.market_id, ts, client_id, order_id);
    }
    JsonObject body {{"address", creds.address}, {"accountIndex", creds.account_index}, {"marketId", market.market_id}, {"timestamp", ts}};
    if (client_id) {
        body["kind"] = "clientId";
        body["clientId"] = *client_id;
    } else {
        body["kind"] = "orderId";
        body["orderId"] = *order_id;
    }
    if (o) {
        o->cancels += 1;
        if (!keep_timing) {  // timed from this cancel on, unless an earlier one is still in flight
            o->cancel_sent.reset();
            o->cancel_sent_ns.reset();
        }
        o->cancel_accepted = false;
        // Arcus answers 202 to a cancel of an order it does not have (yet): one not seen resting
        // gets cancelled once more if the orders channel shows it resting after all.
        o->recancel_on_open = !o->seen_open;
    }
    auto describe = [o, client_id, order_id, why]() -> std::string {
        std::string target = client_id ? "clientId " + *client_id : "orderId " + *order_id;
        std::string text;
        if (!o) text = target;
        else if (o->seen_open) text = std::format("{} ({})", o->str(), target);
        else  // still on its way: the cancel does not wait for it to land
            text = std::format("{} ({}, not acknowledged yet, {:.1f} ms after placing)", o->str(), target, ms_since(o->placed_at));
        return why.empty() ? text : text + ": " + why;
    };
    return post("cancelOrder", body, message, ts, o, std::nullopt, o != nullptr, o ? std::nullopt : order_id, describe, signature);
}

std::vector<RequestPtr> ArcusGateway::schedule_cancel(std::optional<int64_t> deadline_us) {
    int64_t ts = time_ns();
    JsonObject body {{"address", creds.address}, {"accountIndex", creds.account_index}, {"marketId", market.market_id}};
    if (deadline_us) body["time"] = *deadline_us;
    std::string label = deadline_us ? "scheduleCancel" : "disarm";
    return post("scheduleCancel", body, legacy_message(ts, "scheduleCancel", body), ts, nullptr, label);
}

awaitable<void> ArcusGateway::keep_dms_armed() {
    for (;;) {
        if (dms_asked_)  // the last re-arm was never answered
            events->warning("!!! dead man's switch: the re-arm sent {:.0f} s ago got no answer", monotonic() - *dms_asked_);
        dms_asked_ = monotonic();
        schedule_cancel(time_ns() / 1000 + static_cast<int64_t>(opt.dms_timeout * 1'000'000));
        co_await sleep(opt.dms_timeout / 2);
    }
}

awaitable<void> ArcusGateway::watch_pending() {
    // Once a second: a place/cancel that heard nothing, and leftover orders that will not go away.
    // One bad answer must not end it: the order would then stay "pending" and the bot stop quoting
    // until the next reconnect.
    for (;;) {
        co_await sleep(1.0);
        std::exception_ptr error;
        try {
            co_await check_pending();
            co_await check_strays();
        } catch (...) {
            error = std::current_exception();
        }
        if (error) {
            if (co_await is_cancelled()) std::rethrow_exception(error);
            log->error("pending-order check failed; trying again in 1 s: {}", what(error));
        }
    }
}

awaitable<void> ArcusGateway::check_pending() {
    for (const auto& o : working()) co_await check_pending_order(o);
}

awaitable<void> ArcusGateway::check_pending_order(OrderPtr o) {
    if (!is_working(o) || (o->state != OrderState::PENDING_NEW && o->state != OrderState::PENDING_CANCEL)) co_return;
    if (monotonic() - o->since <= PENDING_TIMEOUT_S) co_return;
    if (o->cancels && o->state == OrderState::PENDING_CANCEL && unseen(*o)) {
        settle_unseen(o);  // cancelled, and still nothing from the exchange
        co_return;
    }
    if (o->cancels && o->state == OrderState::PENDING_CANCEL) {
        // It rested, we cancelled it, and the exchange said nothing since: its CANCELED may be
        // lost (cancelled over a lane while the main connection was down, and the snapshot after
        // the reconnect still showed it OPEN; Arcus answers 202 to a cancel of an order it no
        // longer has).
        if (co_await settle_if_gone(o, std::format("its cancel heard nothing for {:.0f}s", PENDING_TIMEOUT_S))) co_return;
        if (!is_working(o) || o->state != OrderState::PENDING_CANCEL) co_return;  // the answer came meanwhile
    }
    events->warning("!!! no answer for {} in {:.0f}s; cancelling by client id", o->str(), PENDING_TIMEOUT_S);
    set_state(*o, OrderState::PENDING_CANCEL);
    send_cancel(o, "no answer", false);
}

awaitable<bool> ArcusGateway::settle_if_gone(OrderPtr o, std::string why) {
    auto resting = co_await resting_on_exchange(o);
    if (!is_working(o) || o->state != OrderState::PENDING_CANCEL || resting != std::optional<bool>(false)) co_return false;
    events->warning("!!! {} is not among the exchange's open orders (GET /v1/openOrders) although {}; settled", o->str(), why);
    finish(o, OrderState::CANCELED, "not open on the exchange");
    on_change();
    co_return true;
}

awaitable<void> ArcusGateway::settle_if_gone_later(OrderPtr o, std::string why) { co_await settle_if_gone(std::move(o), std::move(why)); }

awaitable<void> ArcusGateway::check_strays() {
    // Leftover orders cancelled STRAY_CHECK_S ago and still not gone: forget those the exchange no
    // longer lists as open, cancel again those it does. Until then the gateway is not ready, so this
    // is what lets it trade again.
    double now = monotonic();
    std::vector<std::string> due;
    for (const auto& [oid, filled] : strays_) {
        auto it = stray_since_.find(oid);
        if (now - (it == stray_since_.end() ? now : it->second) >= STRAY_CHECK_S) due.push_back(oid);
    }
    if (due.empty()) co_return;
    auto listed = co_await open_orders();
    if (!listed) {
        for (const auto& oid : due) stray_since_[oid] = monotonic();  // ask again later
        co_return;
    }
    std::set<std::string> open_ids;
    for (const auto& o : *listed)
        if (const Json* id = jget(o, "orderId"); id != nullptr) open_ids.insert(to_str(*id));
    for (const auto& oid : due) {
        if (!strays_.count(oid)) continue;  // settled while we asked
        if (open_ids.count(oid)) {
            events->warning("!!! order {} not placed by this run is still open: cancelling it again", oid);
            stray_since_[oid] = monotonic();
            cancel_by(std::nullopt, oid, nullptr);
        } else {
            events->warning("!!! order {} not placed by this run is not among the exchange's open orders: forgotten", oid);
            drop_stray(oid);
        }
    }
}

void ArcusGateway::spawn(awaitable<void> aw) { Task::spawn(std::move(aw), "background check", log->name()); }

void ArcusGateway::drop_stray(const std::string& order_id) {
    stray_since_.erase(order_id);
    if (strays_.erase(order_id)) {
        check_ready();  // nothing left to wait for
        on_change();
    }
}

awaitable<std::optional<JsonArray>> ArcusGateway::open_orders() {
    if (opt.rest_url.empty()) co_return std::nullopt;
    std::string url = with_params(opt.rest_url + "/v1/openOrders",
                                  {{"address", creds.address}, {"accountIndex", std::to_string(creds.account_index)},
                                   {"market", std::to_string(market.market_id)}, {"status", "OPEN"}});
    HttpResponse resp;
    std::exception_ptr error;
    try {
        std::vector<std::pair<std::string, std::string>> headers {{"X-API-Key", creds.api_key}};
        resp = co_await http_get(url, std::move(headers), OPEN_ORDERS_TIMEOUT_S);
    } catch (...) {
        error = std::current_exception();
    }
    if (error && is_cancellation(error)) {
        bool cancelled = co_await is_cancelled();
        if (cancelled) std::rethrow_exception(error);
    }
    std::optional<JsonArray> out;
    std::string failure = what(error);
    if (!error) {
        try {
            resp.raise_for_status(url);
            Json body = parse_json(resp.body);
            const Json* listed = jget(body, "orders");
            if (listed == nullptr || !listed->is_array()) throw std::invalid_argument("no list of orders in " + resp.body.substr(0, 200));
            out.emplace();
            for (const auto& o : listed->get_array())
                if (o.is_object()) out->push_back(o);
        } catch (const std::exception& err) {
            failure = err.what();
            out.reset();
        }
    }
    if (!out) {
        events->info("--- GET /v1/openOrders failed: {}", failure);
        co_return std::nullopt;
    }
    co_return out;
}

awaitable<std::optional<bool>> ArcusGateway::resting_on_exchange(OrderPtr o) {
    auto listed = co_await open_orders();
    if (!listed) co_return std::nullopt;
    for (const auto& item : *listed) {
        if (jstr(item, "clientId") == o->client_id) co_return true;
        if (o->order_id && jstr(item, "orderId") == o->order_id) co_return true;
    }
    co_return false;
}

bool ArcusGateway::unseen(const Order& o, bool acked_ok) {
    return (acked_ok || !o.order_id) && !o.seen_open && o.filled == 0 && !o.trade_filled();
}

void ArcusGateway::settle_unseen(const OrderPtr& o, bool after_snapshot) {
    // Settle an order the exchange never showed although we cancelled it: the placement never
    // reached the exchange (a dropped connection takes it along), or, after_snapshot, an order no
    // snapshot list holds. Arcus answers a cancel of an order it does not have with 202, so waiting
    // for its CANCELED would wait forever.
    if (is_working(o) && o->state == OrderState::PENDING_CANCEL && unseen(*o, after_snapshot)) {
        std::string where = after_snapshot && o->order_id ? "is gone after the reconnect" : "never reached the exchange";
        events->warning("!!! {} {}: settled", o->str(), where);
        finish(o, OrderState::CANCELED, where);
    }
}

awaitable<void> ArcusGateway::wait_sent(std::vector<RequestPtr> requests, double deadline) {
    while (monotonic() < deadline &&
           std::any_of(requests.begin(), requests.end(), [](const RequestPtr& r) { return !r->sent && !r->failed; }))
        co_await sleep(0.001);
}

awaitable<void> ArcusGateway::shutdown() {
    // Best effort on exit: cancel the working orders, and only once they are gone disarm the
    // switch. An order that may still rest keeps the switch armed, so the exchange cancels it after
    // we are gone (that spends one of the subaccount's daily triggers, which beats an order left for
    // a month). The main connection keeps reading the exchange's answers meanwhile.
    stopping_ = true;
    if (main_conn.ws == nullptr) co_return;
    dms_task_.cancel();  // no more re-arms: the last one stands until we disarm
    double deadline = monotonic() + 3.0;
    bool timed_out = false;
    auto orders_now = working();
    for (const auto& o : orders_now) {
        bool inflight = o->state == OrderState::PENDING_CANCEL && o->cancel_sent.has_value();
        set_state(*o, OrderState::PENDING_CANCEL);
        co_await wait_sent(cancel_by(o->client_id, std::nullopt, o, "shutting down", inflight), deadline);
    }
    if (!orders_now.empty()) {
        while (!orders.empty() && main_conn.ws != nullptr) {  // until the cancels are confirmed
            if (monotonic() >= deadline) {
                timed_out = true;
                break;
            }
            co_await sleep(0.002);
            for (const auto& o : working()) {
                if (o->state != OrderState::PENDING_CANCEL) {  // refused (rate limit, 5xx): again
                    co_await sleep(0.2);
                    set_state(*o, OrderState::PENDING_CANCEL);
                    co_await wait_sent(cancel_by(o->client_id, std::nullopt, o, "shutting down, cancel refused"), deadline);
                }
            }
        }
    }
    if (!timed_out && opt.dms_timeout && orders.empty() && strays_.empty() && main_conn.ws != nullptr)
        co_await wait_sent(schedule_cancel(std::nullopt), deadline);
    if (timed_out) log->warning("shutdown cleanup incomplete: TimeoutError()");
    for (const auto& o : working())
        if (opt.dms_timeout)
            log->warning("{} may still rest: the dead man's switch stays armed and cancels it within {:.0f}s", o->str(), opt.dms_timeout);
}

// --------------------------------------------------------------- messages

void ArcusGateway::on_message(const Json& msg) {
    if (!msg.is_object()) return;
    if (has_key(msg, "method") && has_key(msg, "id")) {
        on_response(msg, main_conn);
        return;
    }
    std::string kind = field(msg, "type"), channel = field(msg, "channel");
    if (kind == "error") {
        std::string text = dump_json(msg);
        log->error("server error: {}", text);
        events->warning("!!! server error: {}", text.substr(0, 300));
        return;
    }
    if (kind == "subscribed" && (channel == "trades" || channel == "userFills")) events->info("<<< subscribed to {}", channel);
    if (channel == "userFills" && kind == "channel_data") {
        const Json* contents = jget(msg, "contents");
        on_user_fills(contents == nullptr ? Json() : *contents);
        return;
    }
    if (channel == "orders" && (kind == "subscribed" || kind == "channel_data")) {
        const Json& contents = object_or_empty(jget(msg, "contents"));
        if (truthy(jget(contents, "isSnapshot"))) on_orders_snapshot(contents);
        else if (kind == "channel_data") on_order_update(contents);
    } else if (channel == "trades" && kind == "channel_data") {
        const Json* contents = jget(msg, "contents");
        on_public_trades(contents != nullptr && contents->is_array() ? contents->get_array() : JsonArray {});
    } else if (channel == "positions" && (kind == "subscribed" || kind == "channel_data")) {
        on_positions(object_or_empty(jget(msg, "contents")));
        if (kind == "subscribed") {
            snapshots_.insert("positions");
            check_ready();
        }
        on_change();
    }
}

void ArcusGateway::on_response(const Json& msg, Conn& conn) {
    int64_t request_id = to_int(*jget(msg, "id"));
    RequestPtr request;
    if (auto it = conn.requests.find(request_id); it != conn.requests.end()) {
        request = it->second;
        conn.requests.erase(it);
    }
    std::string method = request ? request->method : field(msg, "method");
    OrderPtr o = request ? request->order : nullptr;
    int status = static_cast<int>(int_or(msg, "status", 0));
    if (method == "cancelOrder") {
        on_cancel_response(conn, request_id, request, status, msg);
        return;
    }
    if (method == "scheduleCancel") {
        on_dms_response(status, msg);
        return;
    }
    if (method == "disarm") return;
    if (200 <= status && status < 300) {
        const Json& result = object_or_empty(jget(msg, "result"));
        if (method == "placeOrder" && o && truthy(jget(result, "orderId"))) {
            o->order_id = field(result, "orderId");
            if (!o->acked_at) o->acked_at = monotonic();
        }
        return;
    }
    const Json& error = object_or_empty(jget(msg, "error"));
    std::string reason = truthy(jget(error, "message")) ? field(error, "message")
                         : truthy(jget(error, "type")) ? field(error, "type")
                                                       : std::format("status {}", status);
    if (method == "placeOrder" && o) {
        finish(o, OrderState::REJECTED, reason);
        on_change();
    } else {
        log->error("{} failed: {}", method, reason);
    }
}

void ArcusGateway::event_delay(double ms) {
    delays.add(ms);
    push_capped(main_conn.events, std::pair {monotonic(), ms}, 2000);
}

void ArcusGateway::on_cancel_response(Conn& conn, int64_t request_id, const RequestPtr& request, int status, const Json& msg) {
    // How the exchange answered one copy of a cancel, and what to do on a refusal. 202
    // CANCEL_ACKNOWLEDGED is the rule and the orders channel then brings the outcome; a 200 may
    // carry the final state already. "Not found" means the order is gone, or that the cancel
    // overtook a placement still on its way.
    std::optional<double> took_ms;
    if (request) {
        took_ms = ms_since(request->sent.value_or(request->created));
        push_capped(conn.cancel_replies_ms, *took_ms, 200);
        push_capped(conn.replies, std::pair {monotonic(), *took_ms}, 2000);
    }
    const Json* result_raw = jget(msg, "result");
    const Json& result = object_or_empty(result_raw);
    std::string text;
    std::optional<std::string> code;
    bool accepted;
    if (200 <= status && status < 300) {
        std::string outcome = truthy(jget(result, "status")) ? field(result, "status") : "OK";
        if (truthy(jget(result, "rejectionReason"))) code = field(result, "rejectionReason");
        accepted = outcome != "REJECTED";
        text = std::format("{} {}", status, outcome) + (code ? " " + *code : "");
    } else {
        const Json& error = object_or_empty(jget(msg, "error"));
        std::string reason = truthy(jget(error, "message")) ? field(error, "message")
                             : truthy(jget(error, "type")) ? field(error, "type")
                                                           : std::format("status {}", status);
        if (truthy(jget(error, "errorType"))) code = field(error, "errorType");
        else if (truthy(jget(error, "type"))) code = field(error, "type");
        accepted = false;
        text = std::format("{} {}", status, reason) + (code && *code != reason ? " (" + *code + ")" : "");
    }
    std::string flat = lower(text);
    std::replace(flat.begin(), flat.end(), ' ', '_');
    bool not_found = !accepted && (code == "ORDER_NOT_FOUND" || flat.find("not_found") != std::string::npos);
    // Arcus takes each signed timestamp once: when the same request went out on several
    // connections, the copies after the first are refused so.
    bool twin_won = !accepted && request && request->copies > 1 &&
                    (code == "TimestampReused" || lower(text).find("timestamp already used") != std::string::npos);
    if (const Json* limits = jget(result, "rateLimit"); truthy(limits))
        if (const Json* pool = jget(*limits, "remaining"); pool != nullptr && !pool->is_null())
            text += std::format(", cancel pool {} left", to_str(*pool));

    OrderPtr o = request ? request->order : nullptr;
    bool current = o && request && o->state == OrderState::PENDING_CANCEL && request->batch == o->cancels;
    bool refused = current && !accepted && !not_found && !twin_won;
    // Maybe the exchange wants a fresher timestamp than a signature made ahead.
    bool stale = refused && request->presigned && status != 429 && presign_cancel;
    // A copy that found nothing after another copy got through, or after the order was settled,
    // is the expected loser of the race, not a problem.
    bool late = !accepted && not_found && o && (terminal(o->state) || o->cancel_accepted || request->batch != o->cancels);
    // It left before the order was seen resting and found nothing: most likely it overtook the
    // placement (cancels have a faster lane).
    bool overtook = current && not_found && !request->seen_open;
    std::string note;
    if (twin_won) {
        note = "; the same cancel got there first on another connection";
    } else if (stale) {
        note = "; re-sending, signed now, and no longer signing ahead";
    } else if (refused && !o->cancel_accepted && !cancel_in_flight(*o)) {
        note = "; order still open";
    } else if (late) {
        if (terminal(o->state)) note = std::format("; order already {}", state_value(o->state));
        else if (o->cancel_accepted) note = "; another copy got through";
        else note = "; a newer cancel is out";
    } else if (overtook) {
        if (o->seen_open) note = "; it overtook the placement, which has landed since: cancelling again";
        else if (!o->overtook) note = "; it overtook the placement: cancelling again at once, and once more if the order shows up";
        else note = "; it overtook the placement again: cancelling once the order shows up";
    }
    // The losing copy of a cancel another copy got through with (or that found the order settled): half
    // the answers to cancels, and nothing to read in them; the console at DEBUG still has them.
    bool loser = twin_won || (late && o && (terminal(o->state) || o->cancel_accepted));
    events->log(loser ? logging::DEBUG : accepted || late || overtook ? logging::INFO : logging::WARNING, "<<< cancel #{} {} {}{}: {}{}",
                request_id, conn.name, accepted ? "ACCEPTED" : "REFUSED", took_ms ? std::format(" in {:.1f} ms", *took_ms) : "",
                text, note);

    if (request && request->stray) {
        if (not_found && strays_.count(*request->stray)) {
            // Gone: but if it filled just now, its FILLED event may still be on its way (the orders
            // channel trails by up to a second in bursts), so its fills count a moment longer.
            std::string stray = *request->stray;
            call_later(NOT_FOUND_GRACE_S, [this, stray] { drop_stray(stray); });
        }
        return;
    }
    if (!current) return;  // an older cancel, or the order is settled already
    if (accepted) {
        o->cancel_accepted = true;
    } else if (not_found) {
        cancel_found_nothing(o, request);
    } else if (stale) {
        presign_cancel = 0.0;
        o->presigned.reset();
        send_cancel(o, "re-sent, signed now", false);
    } else if (twin_won) {
        // the copy that got there first answers for this cancel
    } else if (!note.empty()) {
        // Refused for another reason (rate limit, validation): decide again after a pause.
        o->cancel_sent.reset();
        o->cancel_sent_ns.reset();
        set_state(*o, o->seen_open ? OrderState::OPEN : OrderState::PENDING_NEW);
        cooldown_until_ = monotonic() + REJECT_COOLDOWN_S;
        call_later(REJECT_COOLDOWN_S, [this] { on_change(); });
    }
}

bool ArcusGateway::cancel_in_flight(const Order& o) const {
    std::vector<const Conn*> conns {&main_conn};
    for (const auto& c : lanes) conns.push_back(c.get());
    for (const Conn* c : conns)
        for (const auto& [id, r] : c->requests)
            if (r->order.get() == &o && r->method == "cancelOrder" && r->batch == o.cancels) return true;
    return false;
}

void ArcusGateway::cancel_found_nothing(const OrderPtr& o, const RequestPtr& request) {
    if (!request->seen_open) {
        // This copy left before the order was seen resting: it may have overtaken the placement
        // (cancels have their own, faster lane).
        if (o->seen_open) {
            send_cancel(o, "the placement landed after the cancel", false);
            return;
        }
        if (!o->overtook) {
            // By now, a round trip later, the placement has most likely landed: cancel again rather
            // than wait for the orders channel to say so. Once only; after that the event decides.
            o->overtook = true;
            send_cancel(o, "the cancel overtook the placement", false);
            return;
        }
        o->recancel_on_open = true;
    }
    // Otherwise the order is gone; the orders channel says whether it filled.
    int batch = o->cancels;
    call_later(NOT_FOUND_GRACE_S, [this, o, batch] { settle_not_found(o, batch); });
}

void ArcusGateway::settle_not_found(const OrderPtr& o, int batch) {
    if (o->state == OrderState::PENDING_CANCEL && o->cancels == batch && !o->cancel_accepted) {
        finish(o, OrderState::CANCELED, "not found");
        on_change();
    }
}

void ArcusGateway::on_dms_response(int status, const Json& msg) {
    dms_asked_.reset();
    if (200 <= status && status < 300) {
        if (!dms_armed_) log->info("dead man's switch armed: orders cancelled after {:.0f}s of silence", opt.dms_timeout);
        dms_armed_ = true;
        check_ready();
        on_change();
        return;
    }
    const Json& error = object_or_empty(jget(msg, "error"));
    dms_armed_ = false;
    ready = false;
    std::string why = truthy(jget(error, "message")) ? field(error, "message")
                      : truthy(jget(error, "type")) ? field(error, "type")
                                                    : std::format("status {}", status);
    log->error("dead man's switch refused ({}): not trading without it; set trading.dms_timeout_s = 0 to trade anyway", why);
    events->warning("!!! dead man's switch refused ({}): not trading without it", why);
    for (const auto& o : working()) cancel(o, "dead man's switch lost");
    on_change();
}

void ArcusGateway::on_orders_snapshot(const Json& contents) {
    // Arcus sends the open orders and the recently closed ones in two separate arrays.
    std::vector<const Json*> entries;
    for (const char* key : {"openOrders", "recentClosedOrders"}) {
        const Json* list = jget(contents, key);
        if (list == nullptr || !list->is_array()) continue;
        for (const auto& o : list->get_array())
            if (o.is_object() && int_or(o, "marketId", -1) == market.market_id) entries.push_back(&o);
    }
    std::map<std::string, OrderPtr> ours;
    for (const auto& o : working()) ours[o->client_id] = o;
    std::set<std::string> found;
    for (const Json* data : entries) {
        auto client_id = jstr(*data, "clientId");
        auto it = client_id ? ours.find(*client_id) : ours.end();
        if (it != ours.end()) {
            const OrderPtr& mine = it->second;
            found.insert(mine->client_id);
            auto filled = filled_of(*data);
            if (filled && *filled < mine->filled) {
                // A snapshot behind what we saw (Arcus has sent stale ones): lowering it would count
                // the difference again when the order's next event repeats it.
                events->warning("!!! the snapshot after the reconnect shows {} filled {}, we saw {}: it is behind", mine->str(),
                                fmt(*filled), fmt(mine->filled));
            } else if (filled) {
                mine->filled = *filled;  // already in the positions snapshot
            }
            on_order_update(*data, true);
        } else if (field(*data, "status") == "OPEN") {
            cancel_stray(*data);
        }
    }
    for (const auto& [client_id, mine] : ours) {
        if (found.count(client_id)) continue;
        if (mine->state == OrderState::PENDING_NEW || (mine->state == OrderState::PENDING_CANCEL && !mine->seen_open)) {
            // The placement may still land: cancel it by client id to be sure. Arcus answers 202 to a
            // cancel of an order it does not have, so that answer proves nothing; with the order in
            // neither the snapshot nor any event by then, it never reached the exchange. Should it
            // land after all, it is a stray.
            set_state(*mine, OrderState::PENDING_CANCEL);
            send_cancel(mine, "not resting after reconnect", false);
            // Listed nowhere, acknowledged or not: once nothing comes of the cancel either, it is
            // gone (any fill of it is in the positions snapshot).
            OrderPtr order = mine;
            call_later(NOT_FOUND_GRACE_S, [this, order] { settle_unseen(order, true); });
        } else {
            // It rested, and no list holds it now: gone while we were disconnected, or the snapshot
            // is behind and it rests still. Cancel it to be sure, then settle it once GET
            // /v1/openOrders no longer lists it.
            if (mine->state == OrderState::OPEN) set_state(*mine, OrderState::PENDING_CANCEL);
            send_cancel(mine, "not listed after reconnect", false);
            OrderPtr order = mine;
            call_later(NOT_FOUND_GRACE_S, [this, order] {
                spawn(settle_if_gone_later(order, "no snapshot list held it after the reconnect"));
            });
        }
    }
    snapshots_.insert("orders");
    check_ready();
    on_change();
}

void ArcusGateway::cancel_stray(const Json& data) {
    auto order_id = text_field(data, "orderId");
    if (!order_id || strays_.count(*order_id)) return;
    strays_[*order_id] = filled_of(data).value_or(Decimal(0));
    stray_since_[*order_id] = monotonic();
    events->warning("!!! cancelling order not placed by this run: {} {} @ {}", field(data, "side"), field(data, "remainingSize"),
                    field(data, "price"));
    cancel_by(std::nullopt, *order_id, nullptr);
}

void ArcusGateway::check_ready() {
    bool now_ready = snapshots_.count("orders") && snapshots_.count("positions") && strays_.empty() &&
                     (dms_armed_ || !opt.dms_timeout);
    if (now_ready && !ready) log->info("trading ready: position {}", fmt(position()));
    ready = now_ready;
}

void ArcusGateway::on_order_update(const Json& data, bool snapshot) {
    if (int_or(data, "marketId", -1) != market.market_id) return;
    std::optional<std::string> status = jstr(data, "status");
    auto client_id = jstr(data, "clientId");
    OrderPtr o;
    for (const auto& [side, w] : orders)
        if (client_id && w->client_id == *client_id) o = w;
    if (!o && client_id) o = recent(*client_id);
    if (!o) {
        on_foreign_update(data, status);
        return;
    }
    if (!o->acked_at && truthy(jget(data, "orderId"))) o->acked_at = monotonic();
    if (!snapshot && clock_offset_ms && truthy(jget(data, "updatedAt"))) {
        try {
            event_delay(wall_time() * 1000 - static_cast<double>(to_int(*jget(data, "updatedAt"))) / 1000 - *clock_offset_ms);
        } catch (const std::invalid_argument&) {
        }
    }
    if (truthy(jget(data, "orderId"))) o->order_id = field(data, "orderId");
    const Json* seq = jget(data, "sequenceNumber");
    std::optional<int64_t> seq_n;
    if (seq != nullptr && !seq->is_null()) seq_n = to_int(*seq);
    if (auto filled = filled_of(data)) {
        std::string when = "first";
        Decimal last_seen;
        for (const auto& [source, marks] : o->seen)
            if (source == "orders channel" && !marks.empty()) last_seen = marks.back().first;
        if (*filled > last_seen) {
            when = seen(*o, "orders channel", *filled);
            if (*filled <= o->filled && !snapshot)  // another source counted it first (not a reconnect gap)
                events->info("<<< orders channel: {} {} @ {} filled {}, {}", o->side, fmt(o->size), fmt(o->price), fmt(*filled), when);
        }
        // userFills and public trades may have shown more already; the larger one counts
        fill(o, min(max(*filled, o->trade_filled()), o->size), text_field(data, "avgFillPrice"), seq_n,
             "seen on the orders channel" + std::string(when == "first" ? "" : ", " + when));
    }
    if (terminal(o->state)) {
        // A late event of an order we have settled: its fills count, and if it is resting after
        // all (a placement that landed late), pull it.
        if (status == "OPEN") {
            forget_recent(o->client_id);
            cancel_stray(data);
        }
        on_change();
        return;
    }
    if (status && (*status == "FILLED" || *status == "CANCELED" || *status == "MARGIN_CANCELED" || *status == "REJECTED") &&
        truthy(jget(data, "updatedAt"))) {
        try {
            o->done_us = to_int(*jget(data, "updatedAt"));
        } catch (const std::invalid_argument&) {
        }
    }
    if (status == "FILLED") {
        finish(o, OrderState::FILLED);
    } else if (status == "CANCELED" || status == "MARGIN_CANCELED") {
        finish(o, OrderState::CANCELED, *status != "CANCELED" ? std::optional(lower(*status)) : std::nullopt, !snapshot);
    } else if (status == "REJECTED") {
        const Json* why = jget(data, "rejectionReason");
        finish(o, OrderState::REJECTED, why != nullptr && !why->is_null() ? std::optional(to_str(*why)) : std::nullopt);
    } else if (status == "OPEN") {
        if (!o->seen_open) {  // resting through a reconnect: in the book long since
            if (snapshot) o->open_at.reset();
            else o->open_at = monotonic();
        }
        o->seen_open = true;
        if (o->state == OrderState::PENDING_NEW) {
            set_state(*o, OrderState::OPEN);
        } else if (o->state == OrderState::PENDING_CANCEL && (o->recancel_on_open || snapshot)) {
            // timed from the first cancel, which may be the one that acts
            send_cancel(o, snapshot ? "still resting after reconnect" : "the placement landed after the cancel", !snapshot);
            if (snapshot) {  // a stale snapshot can list an order the exchange no longer has: ask it soon
                OrderPtr order = o;
                call_later(SNAPSHOT_RECHECK_S, [this, order] {
                    spawn(settle_if_gone_later(order, "the snapshot after the reconnect listed it open and its cancel heard nothing since"));
                });
            }
        }
    }
    on_change();
}

void ArcusGateway::on_public_trades(const JsonArray& trades) {
    // Our fills, seen in the public trades: the maker is our order.
    bool changed = false;
    for (const auto& trade : trades) {
        if (!trade.is_object()) continue;
        OrderPtr o = maker_order(trade);
        if (!o || !truthy(jget(trade, "size"))) continue;
        std::string trade_id = truthy(jget(trade, "tradeId"))
                                   ? field(trade, "tradeId")
                                   : std::format("({}, {})", py_repr(jget(trade, "timestamp")), py_repr(jget(trade, "size")));
        if (o->trade_ids.count(trade_id)) continue;  // delivered twice
        o->trade_ids.insert(trade_id);
        Decimal size = dec(*jget(trade, "size"));
        o->public_filled += size;
        if (truthy(jget(trade, "tradeId"))) o->trades.try_emplace(trade_id, size);  // userFills may have it already
        std::string when = seen(*o, "public trade", o->public_filled);
        Decimal filled = min(max(o->trade_filled(), o->filled), o->size);
        if (filled <= o->filled)  // another source counted it first
            events->info("<<< public trade: {} {} @ {} (id {}, trade {}) filled, {}", o->side, fmt(size), field(trade, "price"),
                         field(trade, "makerOrderId"), trade_id, when);
        fill(o, filled, text_field(trade, "price"), std::nullopt,
             "seen in public trades" + std::string(when == "first" ? "" : ", " + when));
        std::string taker = field(trade, "takerOrderId");
        if (!o->takers.count(taker)) {
            o->takers.insert(taker);
            log_taker(trade, trades);
        }
        if (!terminal(o->state) && o->remaining() <= 0) finish(o, OrderState::FILLED, "seen in public trades");
        changed = true;
    }
    if (changed) on_change();
    if (on_trades && !trades.empty()) on_trades(trades);
}

void ArcusGateway::log_taker(const Json& ours, const JsonArray& trades) {
    // The taker order that hit ours, from its trades in the same message (Arcus sends each taker
    // order's trades together, in the order it took the levels): how many levels it took, where
    // ours was among them and how many ticks from its first price. "at-touch": ours was the only
    // price it took; "sweep down to us" (a seller) / "sweep up to us" (a buyer): it took better
    // prices first and stopped at ours; "sweep through us": it went on past ours.
    const Json* taker = jget(ours, "takerOrderId");
    try {
        std::vector<const Json*> same;
        for (const auto& t : trades) {
            const Json* id = jget(t, "takerOrderId");
            bool equal = (taker == nullptr && id == nullptr) || (taker != nullptr && id != nullptr && *taker == *id);
            if (t.is_object() && equal) same.push_back(&t);
        }
        std::vector<Decimal> prices;
        Decimal total;
        for (const Json* t : same) {
            Decimal price = Decimal::parse(to_str(jget(*t, "price")));
            if (std::find(prices.begin(), prices.end(), price) == prices.end()) prices.push_back(price);
            total += Decimal::parse(to_str(jget(*t, "size")));
        }
        Decimal our_price = Decimal::parse(to_str(jget(ours, "price")));
        auto pos = std::find(prices.begin(), prices.end(), our_price);
        if (pos == prices.end()) return;
        size_t where = static_cast<size_t>(pos - prices.begin()) + 1;
        int64_t ticks = ((our_price - prices[0]).abs() / market.tick_at(our_price)).round_int();
        bool down = field(ours, "side") == "SELL";  // the taker's side: a seller walks the bids down
        std::string kind = prices.size() == 1      ? "at-touch"  // ours was the only price it took
                           : where == prices.size() ? std::format("sweep {} to us", down ? "down" : "up")  // better prices, ours last
                                                    : "sweep through us";  // and went on past ours
        events->info("<<< taker {} {}: {} trades, {} prices {}..{}, {} in all; ours {}, price {} of {}, {} ticks from its first: {}",
                     to_str(taker), field(ours, "side"), same.size(), prices.size(), fmt(prices.front()), fmt(prices.back()),
                     fmt(total), fmt(Decimal::parse(to_str(jget(ours, "size")))), where, prices.size(), ticks, kind);
    } catch (const std::exception&) {
    }
}

void ArcusGateway::on_user_fills(const Json& contents) {
    // Every fill of ours as the exchange matched it. It comes before the orders channel and the
    // public trades, so the fill counts from here, and the strategy hears of it before the line
    // about it is written. Not counted: the fills of a snapshot (the positions snapshot has them
    // already) and those of orders this run did not place.
    if (contents.is_object() && truthy(jget(contents, "isSnapshot"))) {
        const Json* fills = jget(contents, "fills");
        events->info("<<< userFills snapshot of {} fills: not counted, the positions snapshot has them",
                     fills != nullptr && fills->is_array() ? fills->get_array().size() : 0);
        return;
    }
    double received_ms = wall_time() * 1000;  // the lines are written later; the lag is to now
    const Json* fills = contents.is_object() && has_key(contents, "fills") ? jget(contents, "fills") : &contents;
    std::vector<const Json*> list;
    if (fills->is_array()) {
        for (const auto& f : fills->get_array()) list.push_back(&f);
    } else {
        list.push_back(fills);
    }
    struct Line {
        const Json* fill;
        std::string trade_id, when;
    };
    std::vector<Line> lines;
    bool changed = false;
    for (const Json* f : list) {
        if (!f->is_object()) continue;
        const Json* market_id = jget(*f, "marketId");
        if (market_id != nullptr && !market_id->is_null() && to_int(*market_id) != market.market_id) continue;
        OrderPtr o;
        std::vector<OrderPtr> candidates = working();
        for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) candidates.push_back(it->second);
        auto client_id = jstr(*f, "clientId");
        auto order_id = jstr(*f, "orderId");
        for (const auto& c : candidates) {
            if ((client_id && *client_id == c->client_id) || (c->order_id && order_id && *order_id == *c->order_id)) {
                o = c;
                break;
            }
        }
        std::string trade_id = field(*f, "tradeId");
        std::string when = "not one of this run's orders";
        if (o && !o->user_trade_ids.count(trade_id)) {
            o->user_trade_ids.insert(trade_id);
            Decimal size;
            try {
                size = Decimal::parse(to_str(jget(*f, "size")));
            } catch (const std::exception&) {
            }
            if (size > 0) {
                o->user_filled += size;
                if (const Json* id = jget(*f, "tradeId"); id != nullptr && !id->is_null())
                    o->trades.try_emplace(trade_id, size);  // the public trade may have come first
            }
            when = seen(*o, "userFills", o->user_filled);
            if (size > 0) changed = count_user_fill(o, *f, when) || changed;
        } else if (o) {
            when = "again";
        }
        lines.push_back({f, trade_id, when});
    }
    if (changed) on_change();
    for (const auto& [f, trade_id, when] : lines) {
        const Json* created = jget(*f, "createdAt");
        std::string lag;
        if (created != nullptr && is_number(*created) && to_float(*created) > 0) {
            double ms = received_ms - to_float(*created) / 1000;
            if (!clock_offset_ms) {
                lag = std::format(", {:.1f} ms after the match (host clock)", ms);
            } else {
                lag = std::format(", {:.1f} ms after the match (clock offset applied)", ms - *clock_offset_ms);
                event_delay(ms - *clock_offset_ms);
            }
        }
        events->info("<<< userFills {} {} @ {} {} {} (trade {}, order {}, pnl {}, fee {}, left {}){}; {}", field(*f, "side"),
                     field(*f, "size"), field(*f, "price"), field(*f, "role"), field(*f, "positionEffect"), trade_id,
                     field(*f, "orderId"), field(*f, "closedPnl"), field(*f, "fee"), field(*f, "remainingSize"), lag, when);
    }
}

bool ArcusGateway::count_user_fill(const OrderPtr& o, const Json& f, const std::string& when) {
    // Count a fill userFills shows: its order's trades so far, or what is left of the order after it
    // (remainingSize), whichever says more. Whether that was news.
    Decimal filled = o->trade_filled();
    try {
        Decimal left = Decimal::parse(to_str(jget(f, "remainingSize")));
        if (Decimal(0) <= left && left <= o->size) filled = max(filled, o->size - left);
    } catch (const std::exception&) {
    }
    filled = min(filled, o->size);
    if (filled <= o->filled) return false;  // the orders channel or a public trade counted it first
    std::optional<int64_t> seq;
    try {
        if (const Json* s = jget(f, "sequenceNumber"); s != nullptr && !s->is_null()) seq = to_int(*s);
    } catch (const std::invalid_argument&) {
    }
    if (seq && *seq <= snapshot_seq_ && o->placed_at > snapshot_at_) {
        // Not in the positions snapshot, which came before the order existed: the number cannot be
        // one of the account's sequence ids. Count it after the snapshot.
        seq.reset();
    }
    const Json* price = jget(f, "price");
    fill(o, filled, price != nullptr && !price->is_null() ? std::optional(to_str(*price)) : std::nullopt, seq,
         "seen on userFills" + std::string(when == "first" ? "" : ", " + when));
    if (!terminal(o->state) && o->remaining() <= 0) finish(o, OrderState::FILLED, "seen on userFills");
    return true;
}

OrderPtr ArcusGateway::maker_order(const Json& trade) const {
    // Our order that made `trade`: by its orderId, or, while the exchange has not told us that yet,
    // by our address, its side and its price.
    if (int_or(trade, "marketId", market.market_id) != market.market_id) return nullptr;
    auto maker_id = jstr(trade, "makerOrderId");
    std::vector<OrderPtr> candidates = working();
    for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) candidates.push_back(it->second);
    for (const auto& o : candidates)
        if (o->order_id && maker_id && *o->order_id == *maker_id) return o;
    for (const auto& o : working()) {
        if (o->order_id) continue;
        std::string maker = lower(jstr(trade, "makerAddress").value_or(""));
        std::string taker_side = o->side == "BUY" ? "SELL" : "BUY";  // the taker's side
        auto price = Decimal::try_parse(jget(trade, "price") ? to_str(*jget(trade, "price")) : "0");
        if (maker == lower(creds.address) && field(trade, "side") == taker_side && price && *price == o->price) return o;
    }
    return nullptr;
}

void ArcusGateway::on_foreign_update(const Json& data, const std::optional<std::string>& status) {
    auto order_id = text_field(data, "orderId");
    if (order_id && strays_.count(*order_id)) {
        auto filled = filled_of(data);
        Decimal delta = filled ? *filled - strays_[*order_id] : Decimal(0);
        if (delta > 0) {
            strays_[*order_id] = *filled;
            const Json* seq = jget(data, "sequenceNumber");
            add_position(field(data, "side") == "BUY" ? delta : -delta,
                         seq != nullptr && !seq->is_null() ? std::optional(to_int(*seq)) : std::nullopt);
            events->warning("!!! stray order filled {} {} -> position {}", field(data, "side"), fmt(delta), fmt(position()));
            round_trips->sync(position());  // not one of our round trips
        }
        if (status && (*status == "FILLED" || *status == "CANCELED" || *status == "MARGIN_CANCELED" || *status == "REJECTED")) {
            events->info("<<< order {} not placed by this run: {}", *order_id, lower(*status));
            drop_stray(*order_id);
        }
    } else if (status == "OPEN") {
        cancel_stray(data);
    }
}

}  // namespace nmm
