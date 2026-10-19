// Reconnecting websocket feed that maintains one OrderBook (or none: a feed of
// trades passes each one on and keeps no book).
//
// A feed can hold several connections ("links") to the same stream. Every update
// arrives on each of them; the first copy advances the book and later copies are
// dropped by sequence id, so the book follows whichever route is faster at the
// moment. A link that drops or lags does not disturb the book while another one
// is up: the book is invalidated only when every link is down.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "net/net.hpp"
#include "orderbook.hpp"
#include "util/aio.hpp"
#include "util/json.hpp"
#include "util/log.hpp"

namespace nmm {

using BookListener = std::function<void(OrderBook&)>;

// One connection of a feed; every link carries the same stream.
struct Link {
    int index = 0;
    std::shared_ptr<WsConnection> ws;
    std::optional<int64_t> last;  // id of the last update it delivered since connecting
    int64_t wins = 0;  // updates it delivered first
    bool up() const { return ws != nullptr; }
};

struct FeedOptions {
    BookListener on_update;
    double stale_timeout = 30.0;  // reconnect after this long without a data frame; 0 = off
    double heartbeat = 20.0;  // websocket ping/pong (see WsConnection::connect)
    double reconnect_min = 0.5;
    double reconnect_max = 30.0;
    int connections = 1;
};

// Owns the connection lifecycle; subclasses own the book protocol.
//
// run() runs one loop per link: connect, on_connected, read messages into
// on_message until the socket closes, errors, misses a heartbeat pong or goes
// silent for `stale_timeout` seconds (0 disables the silence check), then
// on_disconnected and reconnect with exponential backoff. The book is
// invalidated when no link is left, so readers never price off a frozen book.
class BookFeed {
public:
    BookFeed(std::string name, OrderBook* book, std::string url, FeedOptions options);
    virtual ~BookFeed();
    BookFeed(const BookFeed&) = delete;
    BookFeed& operator=(const BookFeed&) = delete;

    // Every link, until the task running it is cancelled.
    awaitable<void> run();

    int links_up() const;
    // `msg` carries market data (not an ack, a greeting or an app-level pong):
    // only such frames keep a link from going stale.
    virtual bool carries_data(const Json& msg) const { return true; }
    // A frame that is no JSON message at all (OKX's bare "pong"): skipped, neither parsed nor counted.
    virtual bool skips(std::string_view text) const { return false; }
    // Send `payload` on `link` (the first link when null); throws when it is down.
    virtual void send_json(const Json& payload, Link* link = nullptr);
    // Called after each (re)connect of a link, before its first message is read.
    virtual void on_connected(Link& link) {}
    virtual void on_disconnected(Link* link);
    // Handle one decoded JSON message from `link` (the first link if null).
    virtual void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) = 0;

    std::string name;
    std::string url;
    OrderBook* book;
    FeedOptions options;
    Logger& log;
    std::vector<Link> links;
    int64_t connect_count = 0;
    int64_t resync_count = 0;  // book rebuilt after missed updates
    int64_t refresh_count = 0;  // book rebuilt by design, e.g. to extend its depth
    int64_t message_count = 0;

protected:
    bool others_up(const Link& link) const;
    void notify();
    awaitable<void> run_link(Link& link);
};

// Feed whose book is seeded from a REST snapshot spliced with stream updates.
//
// While the book is not synced, stream updates are buffered and a snapshot is
// fetched; the venue's try_sync splices the two once they overlap. resync()
// throws the book away and starts over, without touching the stream
// subscriptions.
//
// Updates from all links are merged by id (ids() gives an update's first and
// last id): a copy the book already has is dropped; an update that starts past
// the book's next id waits ("parked") while another link that is behind can
// still deliver what is missing, and the book is rebuilt only if that does not
// happen within `gap_wait` seconds.
class SnapshotFeed : public BookFeed {
public:
    using BookFeed::BookFeed;
    ~SnapshotFeed() override;

    double snapshot_refetch_delay = 0.25;  // seconds before re-requesting a too-old snapshot
    size_t max_buffered = 10'000;
    double gap_wait = 0.5;  // seconds a parked update waits for a lagging link to fill the gap
    size_t max_parked = 1'000;

    void on_connected(Link& link) override;
    void on_disconnected(Link* link) override;

    // Fetch a book snapshot over REST.
    virtual awaitable<Json> fetch_snapshot() = 0;

    const std::vector<Json>& buffer() const { return buffer_; }

protected:
    // First and last update id an update covers.
    virtual std::pair<int64_t, int64_t> ids(const Json& update) const = 0;
    // Apply an update that continues the book. False means the book was reset.
    virtual bool apply(const Json& update) = 0;
    // Seed the book once snapshot_ and buffer_ overlap.
    virtual void try_sync() = 0;

    void reset();
    // Route one stream update: buffer it while unsynced, else merge it into the book.
    void receive(const Json& update, Link* link);
    // Add an update to a synced book. False means the book was reset.
    bool merge(const Json& update, Link* link);
    bool lagging(const Link& link) const;
    void park(int64_t first, const Json& update, Link& link);
    bool drain();
    void gap_expired();
    void buffer_update(const Json& update);
    // Buffered updates past `last_id`, one copy each, in order (links interleave).
    std::vector<Json> buffered_after(int64_t last_id) const;
    int64_t first_buffered() const;
    // Replay buffered updates over a fresh snapshot. False means the book was reset.
    bool replay(const std::vector<Json>& updates);
    // Rebuild the book from a fresh snapshot, keeping updates for the splice.
    void resync(const Json* update = nullptr, std::vector<Json> more = {});
    void request_snapshot(double delay = 0.0);
    awaitable<void> load_snapshot(double delay);

    std::vector<Json> buffer_;
    std::optional<Json> snapshot_;
    Task snapshot_task_;
    std::map<int64_t, std::pair<Json, Link*>> parked_;  // first id -> (update, link it came on)
    TimerHandle gap_timer_;
};

}  // namespace nmm
