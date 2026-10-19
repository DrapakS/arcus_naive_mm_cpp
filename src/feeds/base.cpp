#include "feeds/base.hpp"

#include <algorithm>
#include <boost/asio/this_coro.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/system/system_error.hpp>
#include <format>
#include <stdexcept>

#include "util/clock.hpp"

namespace nmm {

namespace {

// With a stale timeout, a link that brought no data for that long is dropped:
// `fresh` is set on every frame with data, and one timer, firing about once per
// timeout, closes the connection once nothing came for that long (the read then
// fails). A timeout around every read would cost a few µs a frame. The deadline
// counts data frames only: pings, pongs and app-level pongs do not move it, so a
// stream that stopped while the server still answers pings is noticed.
struct StaleWatch {
    struct State {
        double fresh = 0.0;  // when the last data came (or the link connected)
        double timeout = 0.0;
        bool fired = false;
        std::shared_ptr<WsConnection> ws;
        TimerHandle timer;
    };

    StaleWatch(std::shared_ptr<WsConnection> ws, double timeout) : state(std::make_shared<State>()) {
        state->fresh = monotonic();
        state->timeout = timeout;
        state->ws = std::move(ws);
        schedule(state, timeout);
    }
    ~StaleWatch() { stop(); }
    StaleWatch(const StaleWatch&) = delete;
    StaleWatch& operator=(const StaleWatch&) = delete;

    void stop() {
        state->timer.cancel();
        state->ws.reset();
    }

    static void schedule(const std::shared_ptr<State>& s, double delay) {
        s->timer = call_later(delay, [s] { check(s); });
    }

    static void check(const std::shared_ptr<State>& s) {
        if (s->ws == nullptr) return;
        double now = monotonic();
        double due = s->fresh + s->timeout;
        if (due > now) {
            schedule(s, due - now);
        } else {
            s->fired = true;  // dropped: the read fails at once
            s->ws->close();
        }
    }

    std::shared_ptr<State> state;
};

std::string describe(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}

bool is_timeout(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const boost::system::system_error& e) {
        return e.code() == boost::beast::error::timeout || e.code() == boost::asio::error::timed_out;
    } catch (...) {
        return false;
    }
}

}  // namespace

BookFeed::BookFeed(std::string name, OrderBook* book, std::string url, FeedOptions options)
    : name(std::move(name)),
      url(std::move(url)),
      book(book),
      options(std::move(options)),
      log(get_logger("naive_mm." + this->name)) {
    if (this->options.connections < 1) throw std::invalid_argument("a feed needs at least one connection");
    for (int i = 0; i < this->options.connections; ++i) links.push_back(Link {i, nullptr, std::nullopt, 0});
}

BookFeed::~BookFeed() = default;

awaitable<void> BookFeed::run() {
    std::vector<Task> tasks;
    for (auto& link : links) tasks.push_back(Task::spawn(run_link(link), name, "naive_mm." + name));
    try {
        Event forever;
        co_await forever.wait();
    } catch (...) {
        for (auto& task : tasks) task.cancel();
        throw;
    }
}

awaitable<void> BookFeed::run_link(Link& link) {
    std::string tag = links.size() > 1 ? std::format(" [link {}/{}]", link.index + 1, links.size()) : "";
    double backoff = options.reconnect_min;
    for (;;) {
        bool received = false, closed_by_server = false;
        int close_code = 0;
        std::exception_ptr error;
        std::unique_ptr<StaleWatch> stale;
        try {
            auto ws = co_await WsConnection::connect(url, options.heartbeat);
            link.ws = ws;
            link.last.reset();
            ++connect_count;
            log.info("connected {}{}", url, tag);
            on_connected(link);
            if (options.stale_timeout > 0) stale = std::make_unique<StaleWatch>(ws, options.stale_timeout);
            FrameParser parser;
            for (;;) {
                auto text = co_await ws->read();
                if (!text) break;
                const Json& data = parser.parse(*text);
                if (carries_data(data)) {  // a greeting or an ack is no sign of health
                    received = true;
                    if (stale) stale->state->fresh = monotonic();
                }
                ++message_count;
                on_message(data, now_us(), &link);
            }
            closed_by_server = true;
            close_code = ws->close_code();
        } catch (...) {
            error = std::current_exception();
        }
        bool timed_out = stale && stale->state->fired;
        stale.reset();
        auto cancel_state = co_await boost::asio::this_coro::cancellation_state;
        bool cancelled = cancel_state.cancelled() != boost::asio::cancellation_type::none;
        if (!cancelled) {
            if (closed_by_server) {
                log.warning("connection closed by server (code={}){}", close_code ? std::to_string(close_code) : "None", tag);
            } else if (timed_out || is_timeout(error)) {
                log.warning("no data for {:.0f}s or heartbeat timed out{}", options.stale_timeout, tag);
            } else {
                log.warning("connection error{}: {}", tag, describe(error));
            }
        }
        if (link.ws) link.ws->close();
        link.ws.reset();
        link.last.reset();
        on_disconnected(&link);
        if (cancelled) co_return;
        if (received) backoff = options.reconnect_min;
        log.info("reconnecting in {:.1f}s{}", backoff, tag);
        co_await sleep(backoff);
        backoff = std::min(backoff * 2.0, options.reconnect_max);
    }
}

int BookFeed::links_up() const {
    return static_cast<int>(std::count_if(links.begin(), links.end(), [](const Link& l) { return l.up(); }));
}

bool BookFeed::others_up(const Link& link) const {
    return std::any_of(links.begin(), links.end(), [&](const Link& other) { return &other != &link && other.up(); });
}

void BookFeed::send_json(const Json& payload, Link* link) {
    Link& target = link != nullptr ? *link : links[0];
    if (!target.ws) throw std::runtime_error("not connected");
    target.ws->send(dump_json(payload));
}

void BookFeed::notify() {
    if (options.on_update && book != nullptr) options.on_update(*book);
}

void BookFeed::on_disconnected(Link* link) {
    if (link != nullptr && others_up(*link)) return;  // the other links keep the book current
    book->invalidate();
    notify();
}

// ---------------------------------------------------------------- SnapshotFeed

SnapshotFeed::~SnapshotFeed() { gap_timer_.cancel(); }

void SnapshotFeed::on_connected(Link& link) {
    if (!others_up(link)) reset();
}

void SnapshotFeed::on_disconnected(Link* link) {
    if (link != nullptr && others_up(*link)) return;
    reset();
    BookFeed::on_disconnected(link);
}

void SnapshotFeed::reset() {
    buffer_.clear();
    snapshot_.reset();
    snapshot_task_.cancel();
    snapshot_task_ = Task();
    parked_.clear();
    gap_timer_.cancel();
    gap_timer_ = TimerHandle();
}

void SnapshotFeed::receive(const Json& update, Link* link) {
    Link& from = link != nullptr ? *link : links[0];
    from.last = ids(update).second;
    if (book->synced) merge(update, &from);
    else buffer_update(update);
}

bool SnapshotFeed::merge(const Json& update, Link* link) {
    auto [first, last] = ids(update);
    if (last <= book->seq) return true;  // another link delivered it first
    if (first > book->seq + 1) {
        if (link != nullptr && lagging(*link) && parked_.size() < max_parked) {
            park(first, update, *link);
            return true;
        }
        log.warning("missed updates: book at {}, next update starts at {}; resyncing", book->seq, first);
        ++resync_count;
        resync(&update);
        return false;
    }
    if (link != nullptr) ++link->wins;
    if (!apply(update)) return false;
    return drain();
}

bool SnapshotFeed::lagging(const Link& link) const {
    // another link is up and has not delivered the book's next update yet
    return std::any_of(links.begin(), links.end(), [&](const Link& other) {
        return &other != &link && other.up() && other.last && *other.last <= book->seq;
    });
}

void SnapshotFeed::park(int64_t first, const Json& update, Link& link) {
    parked_.emplace(first, std::make_pair(keep(update), &link));
    if (!gap_timer_) gap_timer_ = call_later(gap_wait, [this] { gap_expired(); });
}

bool SnapshotFeed::drain() {
    while (!parked_.empty()) {
        auto it = parked_.begin();
        if (it->first > book->seq + 1) return true;  // the gap is still open
        auto [update, link] = std::move(it->second);
        parked_.erase(it);
        if (ids(update).second <= book->seq) continue;
        ++link->wins;
        if (!apply(update)) return false;
    }
    gap_timer_.cancel();
    gap_timer_ = TimerHandle();
    return true;
}

void SnapshotFeed::gap_expired() {
    gap_timer_ = TimerHandle();
    if (parked_.empty()) return;
    log.warning("no link filled the gap after {} in {:.1f}s; resyncing", book->seq, gap_wait);
    ++resync_count;
    std::vector<Json> more;
    for (auto& [first, entry] : parked_) more.push_back(entry.first);
    resync(nullptr, std::move(more));
}

void SnapshotFeed::buffer_update(const Json& update) {
    buffer_.push_back(keep(update));
    if (buffer_.size() > max_buffered) buffer_.erase(buffer_.begin());
    if (snapshot_) try_sync();
    else if (!snapshot_task_) request_snapshot();
}

std::vector<Json> SnapshotFeed::buffered_after(int64_t last_id) const {
    std::map<std::pair<int64_t, int64_t>, Json> unique;
    for (const auto& u : buffer_) {
        auto key = ids(u);
        if (key.second > last_id) unique.insert_or_assign(key, u);
    }
    std::vector<Json> out;
    out.reserve(unique.size());
    for (auto& [key, u] : unique) out.push_back(std::move(u));
    return out;
}

int64_t SnapshotFeed::first_buffered() const {
    int64_t first = ids(buffer_.front()).first;
    for (const auto& u : buffer_) first = std::min(first, ids(u).first);
    return first;
}

bool SnapshotFeed::replay(const std::vector<Json>& updates) {
    for (const auto& update : updates)
        if (!merge(update, nullptr)) return false;
    return true;
}

void SnapshotFeed::resync(const Json* update, std::vector<Json> more) {
    std::optional<Json> keep;
    if (update != nullptr) keep = nmm::keep(*update);  // it may live in what reset() clears
    book->invalidate();
    reset();
    if (keep) buffer_.push_back(std::move(*keep));
    for (auto& u : more) buffer_.push_back(std::move(u));
    request_snapshot();
    notify();
}

void SnapshotFeed::request_snapshot(double delay) {
    snapshot_task_.cancel();
    snapshot_task_ = Task::spawn(load_snapshot(delay), name + "-snapshot", "naive_mm." + name);
}

awaitable<void> SnapshotFeed::load_snapshot(double delay) {
    double backoff = 1.0;
    if (delay > 0) co_await sleep(delay);
    Json snapshot;
    for (;;) {
        std::string error;
        try {
            snapshot = co_await fetch_snapshot();
            break;
        } catch (const std::exception& e) {
            error = e.what();
        }
        co_await check_cancelled();
        log.warning("snapshot request failed: {}; retry in {:.0f}s", error, backoff);
        co_await sleep(backoff);
        backoff = std::min(backoff * 2.0, 30.0);
    }
    snapshot_task_ = Task();
    snapshot_ = std::move(snapshot);
    try_sync();
}

}  // namespace nmm
