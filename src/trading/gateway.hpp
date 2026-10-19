// Order gateways: one working order per side at most, plus the position they build.
//
// Gateway holds the bookkeeping shared by the live and paper gateways: the
// working orders, fills, position and counters. One side at a time is what the
// long and short strategies use; the two-sided one (direction = "both") keeps a
// buy and a sell working together. ArcusGateway talks to Arcus over websockets:
// it signs placeOrder/cancelOrder, follows the orders and positions channels of
// our subaccount, and keeps a dead man's switch armed so orders die with the process.
//
// Cancels come first. A cancel goes out by clientId as soon as the strategy asks,
// even before the placement is acknowledged, and it is written to every cancel
// lane at once: connections to the same endpoint take different routes, and the
// first copy to reach the exchange wins. The request is on the wire before
// cancel returns; logging comes after.
//
// The strategy calls place/cancel synchronously. Every change of the working
// order or position calls on_change so the strategy re-evaluates.
//
// Fills are taken from three channels, whichever tells first: userFills (each fill
// of ours as it is matched), the orders channel, and the public trades (a trade
// whose maker is our order). The orders channel counts cumulatively per order,
// the other two per trade (by tradeId, each trade once across both); the largest
// total wins, so a fill seen on all three is counted once.
//
// Order events go to one logger, naive_mm.orders, the moment they happen. Each
// starts with a marker: ">>>" for what we send (PLACE, CANCEL), "<<<" for what the
// exchange answers, "!!!" for trouble.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "health.hpp"
#include "trading/credentials.hpp"
#include "trading/market.hpp"
#include "trading/roundtrip.hpp"
#include "util/aio.hpp"
#include "util/clock.hpp"
#include "util/decimal.hpp"
#include "util/json.hpp"
#include "util/log.hpp"

namespace nmm {

class WsConnection;

constexpr int64_t GOOD_TIL_US = 35LL * 86'400 * 1'000'000;  // Arcus wants goodTilTime at least a month out
constexpr double PENDING_TIMEOUT_S = 10.0;  // a place/cancel without any answer for this long is re-cancelled
constexpr double REJECT_COOLDOWN_S = 0.5;  // pause after a rejected order so a stale book cannot spin us
// Rejections in a row past this many double the pause each time, up to the maximum: a placement
// the exchange keeps refusing (margin, a market gone offline) must not drain the order pool every
// bot on the subaccount shares at two a second.
constexpr int REJECTS_BEFORE_BACKOFF = 3;
constexpr double REJECT_COOLDOWN_MAX_S = 30.0;
// A leftover order we cancelled that has not gone away this long is looked up in GET
// /v1/openOrders: Arcus answers 202 to a cancel of an order it no longer has, and a snapshot
// after a reconnect can list as OPEN an order cancelled long since.
constexpr double STRAY_CHECK_S = 5.0;
constexpr double RECONNECT_RESET_S = 10.0;  // a connection that lived this long reconnects at once, however it ended
// A connection whose answers come slow (cancel replies; on main also the orders channel and
// userFills events, from the exchange's stamp) is reconnected: after an Arcus-wide reconnect some
// connections land on a path ~80-95 ms slower than the rest and stay there for hours. Judged on the
// median of the last SLOW_WINDOW_S, with at least SLOW_MIN_SAMPLES, a connection up that long, one
// at a time while every other is up, with nothing in flight on it, at most once per SLOW_RECONNECT_GAP_S.
constexpr double SLOW_WINDOW_S = 60.0;
constexpr size_t SLOW_MIN_SAMPLES = 10;
constexpr double SLOW_RECONNECT_GAP_S = 300.0;
constexpr double SLOW_CHECK_S = 5.0;
// An acknowledged placement this old is in the public book even when the orders channel (tens
// of ms behind the book, sometimes hundreds) has not said OPEN yet.
constexpr double ACK_IN_BOOK_S = 0.05;
// The orders channel can also say OPEN a few ms BEFORE the public book shows the order: an
// order is taken to be in the book this long after OPEN, unless the book has shown it already.
constexpr double OPEN_IN_BOOK_S = 0.02;
// The orders channel can report our order CANCELED a moment before the public book drops its
// level: for this long the level is still taken to be ours.
constexpr double STALE_LEVEL_S = 0.1;
constexpr double POSITION_SETTLE_S = 1.0;  // a streaming position update is compared with ours once updates stop this long
constexpr double NOT_FOUND_GRACE_S = 2.0;  // a cancel that found nothing waits this long for the orders channel
constexpr double OPEN_ORDERS_TIMEOUT_S = 5.0;  // GET /v1/openOrders, asked when a cancel of a resting order hears nothing
// A reconnect snapshot that lists as OPEN an order we were cancelling: its cancel is sent again,
// and if nothing has come of it this long after, GET /v1/openOrders is asked at once rather than
// after PENDING_TIMEOUT_S (Arcus has sent stale snapshots of orders a lane had already cancelled).
constexpr double SNAPSHOT_RECHECK_S = 1.0;
constexpr size_t RECENT_ORDERS = 16;  // finished orders whose late orders-channel events still count

enum class OrderState { PENDING_NEW, OPEN, PENDING_CANCEL, FILLED, CANCELED, REJECTED };

const char* state_value(OrderState state);  // "pending new", "open", ...
const char* state_name(OrderState state);  // "PENDING_NEW", "OPEN", ...
bool terminal(OrderState state);  // FILLED, CANCELED or REJECTED

struct Order {
    Order(std::string client_id, std::string side, Decimal price, Decimal size);

    std::string client_id;
    std::string side;  // "BUY" | "SELL"
    Decimal price;
    Decimal size;
    OrderState state = OrderState::PENDING_NEW;
    Decimal filled;
    std::optional<std::string> order_id;
    std::optional<std::string> reason;
    double since;  // when the state last changed (monotonic)
    bool seen_open = false;  // the orders channel has shown it resting
    std::optional<double> open_at;  // when it first did (monotonic); none: long before we looked
    std::optional<double> book_base;  // the size the public book showed at its price when it was placed
    bool book_seen = false;  // the public book has shown it: its level grew by its size since then
    int cancels = 0;  // cancels sent for it; each goes out on every cancel lane
    std::optional<double> cancel_sent;  // monotonic time the latest cancel first hit a socket
    std::optional<int64_t> cancel_sent_ns;  // the same on the wall clock, to set against the exchange's times
    std::optional<int64_t> done_us;  // the exchange's time (updatedAt, µs) of the event that finished it
    bool cancel_accepted = false;  // some copy of the latest cancel was accepted
    bool recancel_on_open = false;  // a cancel overtook the placement: cancel again once it rests
    bool overtook = false;  // a cancel of it overtook the placement; one more went out at once
    double placed_at;
    std::optional<double> acked_at;  // when the exchange first named it (placement reply or orders channel)
    Decimal public_filled;  // filled as seen in public trades (each tradeId once)
    std::set<std::string> trade_ids;
    // When each source (orders channel, public trade, userFills) showed it filled how much:
    // (cumulative filled, monotonic time), to time the sources per fill; in the order they came.
    std::vector<std::pair<std::string, std::vector<std::pair<Decimal, double>>>> seen;
    Decimal user_filled;  // filled as seen on userFills (each tradeId once)
    std::set<std::string> user_trade_ids;
    // Its trades by tradeId from either trade-level source (userFills, public trades), each once:
    // two partial fills, one seen on each source so far, add up.
    std::map<std::string, Decimal> trades;
    std::set<std::string> takers;  // taker orders that hit it, each logged once
    // its cancel signed ahead: (timestamp ns, signature, monotonic)
    std::optional<std::tuple<int64_t, std::string, double>> presigned;

    Decimal remaining() const { return size - filled; }
    // Filled as the trade-level sources show it (a trade without a tradeId counts only toward its own source's total).
    Decimal trade_filled() const;
    // Possibly in the exchange book right now.
    bool resting() const { return state == OrderState::OPEN || state == OrderState::PENDING_CANCEL; }
    // The public book can be showing it: the book has shown it (its level grew by its size), the
    // orders channel has shown it resting at least OPEN_IN_BOOK_S ago, or the placement was
    // acknowledged at least ACK_IN_BOOK_S ago. Before that a level at its price belongs to others.
    bool in_book() const;
    // "BUY 0.001 @ 83080.1 [open, filled 0.0004]"
    std::string str() const;
};
using OrderPtr = std::shared_ptr<Order>;

class Gateway {
public:
    explicit Gateway(MarketSpec market, std::function<void()> on_change = {});
    virtual ~Gateway() = default;

    std::string name = "gateway";
    bool orders_in_public_book = true;  // our orders show up in the public Arcus book (not so for paper orders)
    MarketSpec market;
    std::function<void()> on_change;
    std::map<std::string, OrderPtr> orders;  // side -> the working order there; gone once it is done
    bool ready = false;
    std::map<std::string, int64_t> stats;
    Logger* log;  // connections, readiness, the switch
    Logger* events;  // what happens to our orders
    std::unique_ptr<RoundTrips> round_trips;  // one line per trip
    std::optional<double> clock_offset_ms;  // host clock minus the exchange's, when measured
    // ms from the exchange's stamp on an event of our orders to our reading it, clock offset applied
    Delays delays;
    // Called on every fill, after its FILL line: (order, size filled now).
    std::function<void(const OrderPtr&, const Decimal&)> on_fill;
    // Called on every frame of the market's public trades, after our fills in it: its rows (one taker order).
    std::function<void(const JsonArray&)> on_trades;
    // The position as the strategy sees it: its mean entry price and since when it has been held
    // (reset when it goes flat or flips). A position found at start takes the exchange's average
    // entry and the time it was found.
    Clock clock = monotonic;
    std::optional<Decimal> entry_price;
    std::optional<double> position_since;

    // ---------------------------------------------------------- strategy API
    virtual Decimal position() const { return position_; }
    // The working order: the one there is, the latest placed when both sides work.
    OrderPtr order() const;
    // The one working order (tests, and a strategy of one side at a time).
    void set_order(OrderPtr order);
    // The working orders, buy first.
    std::vector<OrderPtr> working() const;
    bool is_working(const OrderPtr& order) const;
    // A cancel can go out now. Placing waits for `ready`; pulling an order must not.
    virtual bool can_cancel() const { return ready; }
    // Pausing after a rejection or a refused cancel.
    bool cooling_down() const;
    // True while a request is in flight or after a rejection.
    bool busy() const;
    // Place an order; `mid` (the Arcus mid now) only feeds the round-trip log.
    OrderPtr place(const std::string& side, const Decimal& price, const Decimal& size, const std::string& why = "",
                   std::optional<double> mid = {});
    // Pull `order`; a placement not yet acknowledged can be cancelled too.
    void cancel(const OrderPtr& order, const std::string& why = "");
    // A position the exchange reports that our fills did not build (at start, after a reconnect).
    void found_position(std::optional<Decimal> entry);
    // (price, size) of our order on `side` that rested and was cancelled less than STALE_LEVEL_S
    // ago: the public book may still show it.
    std::optional<std::pair<double, double>> recently_cancelled(const std::string& side) const;
    void forget_cancelled(const std::string& side);
    virtual std::string status() const;
    virtual awaitable<void> run() = 0;
    // On the way out: cancel what works and wait for it (the live gateway); nothing by default.
    virtual awaitable<void> shutdown() { co_return; }

protected:
    void set_state(Order& order, OrderState state);
    // Record cumulative `filled` for `order`; only the increase counts.
    void fill(const OrderPtr& order, const Decimal& filled, std::optional<std::string> price = {},
              std::optional<int64_t> seq = {}, const std::string& source = "");
    virtual void add_position(const Decimal& delta, std::optional<int64_t> seq);
    void track_entry(const Decimal& before, const Decimal& price);
    // `fresh`: the event is happening now, not a snapshot's record of one.
    void finish(const OrderPtr& order, OrderState state, std::optional<std::string> reason = {}, bool fresh = true);
    // Note that `source` shows `order` filled `filled`; how long after the other sources showed as much, or "first".
    std::string seen(Order& order, const std::string& source, const Decimal& filled);
    // A finished order by clientId (the last RECENT_ORDERS), or null.
    OrderPtr recent(const std::string& client_id) const;
    void forget_recent(const std::string& client_id);

    virtual void send_place(const OrderPtr& order) = 0;
    virtual void send_cancel(const OrderPtr& order, const std::string& why = "") = 0;

    Decimal position_;
    double cooldown_until_ = 0.0;
    int rejects_ = 0;  // placements rejected in a row
    int64_t next_id_ = 1;
    std::vector<std::pair<std::string, OrderPtr>> recent_;  // finished orders by clientId, oldest first
    // side -> (price, remaining, when) of the last resting order cancelled there
    std::map<std::string, std::tuple<Decimal, Decimal, double>> gone_;
};

// One request on one connection.
struct Request {
    std::string method;  // API method, or a label such as "disarm"
    OrderPtr order;
    std::optional<std::string> stray;  // orderId of a leftover order this cancel is for
    double created = monotonic();
    std::optional<double> sent;  // when the frame was written to the socket
    bool failed = false;  // it could not be written
    bool seen_open = false;  // cancels: the order was known to rest when this went out
    int batch = 0;  // cancels: which cancel of the order this copy belongs to
    bool presigned = false;  // signed ahead of time rather than when sent
    int copies = 1;  // connections the same signed request went out on
};
using RequestPtr = std::shared_ptr<Request>;

// One websocket to the exchange: its own request ids and requests in flight.
struct Conn {
    explicit Conn(std::string name) : name(std::move(name)) {}
    std::string name;
    std::shared_ptr<WsConnection> ws;
    int64_t next_id = 1;
    std::map<int64_t, RequestPtr> requests;
    std::deque<double> cancel_replies_ms;  // the last 200
    // (monotonic time, ms) of this connection's answers: cancel replies, and our order events on main
    std::deque<std::pair<double, double>> replies, events;  // the last 2000 each
    double connected_at = 0.0;
    double slow_reconnect_at = -1e300;  // when it was last reconnected for being slow

    void attach(std::shared_ptr<WsConnection> conn);
    // What is slow on it over the last SLOW_WINDOW_S: (what, median ms, samples), or none.
    std::optional<std::tuple<std::string, double, size_t>> slow(double now, double threshold_ms) const;
};

class ArcusGateway : public Gateway {
public:
    struct Options {
        std::string ws_url;
        double dms_timeout = 60.0;
        int cancel_lanes = 0;
        double presign_cancel = 0.0;
        std::string rest_url;  // for GET /v1/openOrders; "" = never asked
        double slow_reconnect_ms = 0.0;  // reconnect a connection answering slower than this; 0 = never
    };

    ArcusGateway(MarketSpec market, ArcusCredentials credentials, Options options, std::function<void()> on_change = {});

    bool can_cancel() const override;
    std::string status() const override;
    Decimal position() const override;
    awaitable<void> run() override;
    awaitable<void> shutdown() override;

    // ------------------------------------------------- message handling (tests drive these)
    void on_message(const Json& msg);
    void on_response(const Json& msg, Conn& conn);
    // Reconnect the first connection found slow, if it may be now; returns it.
    Conn* check_slow(double now);

    ArcusCredentials creds;
    Options opt;
    Conn main_conn {"main"};  // subscriptions, placements, the switch, and cancels
    std::vector<std::unique_ptr<Conn>> lanes;  // cancels only
    double presign_cancel;  // signs ahead while > 0 (set to 0 when the exchange refuses a presigned cancel)

protected:
    void add_position(const Decimal& delta, std::optional<int64_t> seq) override;
    void send_place(const OrderPtr& order) override;
    void send_cancel(const OrderPtr& order, const std::string& why = "") override { send_cancel(order, why, false); }

private:
    friend struct ArcusGatewayTestAccess;

    void send_cancel(const OrderPtr& order, const std::string& why, bool keep_timing);
    awaitable<void> run_main();
    awaitable<void> run_lane(Conn* conn);
    awaitable<void> watch_slow();
    void drop(Conn& conn);
    // Main connection lost: we no longer see fills, so pull the orders through a lane.
    void pull_while_blind();
    void send_frame(Conn& conn, const Json& payload);
    // Sign once (unless `signature` is given) and write the request to the main connection, or to
    // every connection that is up when `lanes` is set.
    std::vector<RequestPtr> post(const std::string& method, const Json& body, const std::string& message, int64_t ts,
                                 const OrderPtr& order, std::optional<std::string> label = {}, bool to_lanes = false,
                                 std::optional<std::string> stray = {}, std::function<std::string()> describe = {},
                                 std::optional<std::string> signature = {});
    void on_send_done(Conn& conn, int64_t request_id, const RequestPtr& request, const std::string& error);
    // Sign the cancel of `order` now, so cancelling it later skips the signature.
    void presign(const OrderPtr& order);
    awaitable<void> keep_presigned();
    // Cancel by clientId (our order, on every lane) or by orderId (a leftover, main only).
    std::vector<RequestPtr> cancel_by(std::optional<std::string> client_id, std::optional<std::string> order_id,
                                      const OrderPtr& order, const std::string& why = "", bool keep_timing = false);
    std::vector<RequestPtr> schedule_cancel(std::optional<int64_t> deadline_us);
    awaitable<void> keep_dms_armed();
    awaitable<void> watch_pending();
    awaitable<void> check_pending();
    awaitable<void> check_pending_order(OrderPtr order);
    // Settle `order`, which we are cancelling, as cancelled if GET /v1/openOrders does not list it.
    awaitable<bool> settle_if_gone(OrderPtr order, std::string why);
    awaitable<void> check_strays();
    // Run in the background, its failure logged.
    void spawn(awaitable<void> aw);
    void drop_stray(const std::string& order_id);
    awaitable<std::optional<JsonArray>> open_orders();
    awaitable<std::optional<bool>> resting_on_exchange(OrderPtr order);
    // The exchange has never shown it: no orders-channel event, no fill, and no placement reply either (unless `acked_ok`).
    static bool unseen(const Order& order, bool acked_ok = false);
    void settle_unseen(const OrderPtr& order, bool after_snapshot = false);
    awaitable<void> wait_sent(std::vector<RequestPtr> requests, double deadline);

    void event_delay(double ms);
    void on_cancel_response(Conn& conn, int64_t request_id, const RequestPtr& request, int status, const Json& msg);
    bool cancel_in_flight(const Order& order) const;
    void cancel_found_nothing(const OrderPtr& order, const RequestPtr& request);
    void settle_not_found(const OrderPtr& order, int batch);
    void on_dms_response(int status, const Json& msg);
    void on_orders_snapshot(const Json& contents);
    void cancel_stray(const Json& data);
    void check_ready();
    void on_order_update(const Json& data, bool snapshot = false);
    void on_public_trades(const JsonArray& trades);
    void log_taker(const Json& ours, const JsonArray& trades);
    void on_user_fills(const Json& contents);
    bool count_user_fill(const OrderPtr& order, const Json& fill, const std::string& when);
    OrderPtr maker_order(const Json& trade) const;
    void on_foreign_update(const Json& data, const std::optional<std::string>& status);
    void on_positions(const Json& contents);
    void check_position();

    std::set<std::string> snapshots_;
    std::map<std::string, Decimal> strays_;  // foreign open orders being cancelled -> filled
    std::map<std::string, double> stray_since_;  // when each was last cancelled (monotonic)
    Task dms_task_, watchdog_, presigner_, main_task_;
    std::vector<Task> lane_tasks_;
    std::optional<double> dms_asked_;  // when the switch request still unanswered went out
    bool dms_armed_ = false;
    bool stopping_ = false;
    // Position = the positions snapshot taken on (re)connect plus every fill in this market after
    // it, by the account sequence id the fill carries (userFills and the orders channel do; public
    // trades do not). Fills are cumulative per order, so they are exact; positions updates are only
    // used to cross-check.
    Decimal snapshot_position_;
    int64_t snapshot_seq_ = -1;
    double snapshot_at_ = 1e300;  // when that snapshot came (monotonic)
    std::vector<std::pair<int64_t, Decimal>> fills_;  // (account sequence, signed size)
    std::optional<Decimal> exchange_position_;  // the latest streaming position update
    TimerHandle position_check_;
    std::optional<std::pair<Decimal, Decimal>> position_warned_;
};

}  // namespace nmm
