#include "feeds/binance.hpp"

#include <format>

#include "util/pyfmt.hpp"

namespace nmm {

namespace {

std::string stream_url(const std::string& symbol, const std::string& ws_url, int update_speed_ms, bool book_ticker) {
    std::string depth = lower(symbol) + "@depth";
    if (update_speed_ms != 1000) depth += std::format("@{}ms", update_speed_ms);
    std::string streams = depth;
    if (book_ticker) streams += "/" + lower(symbol) + "@bookTicker";
    // Earlier configs pointed at the raw-stream path; keep accepting it.
    std::string_view base = remove_suffix(remove_suffix(rstrip(ws_url, '/'), "/ws"), "/stream");
    return std::string(base) + "/stream?streams=" + streams;
}

}  // namespace

BinanceSpotBookFeed::BinanceSpotBookFeed(OrderBook& book, std::string symbol, std::string ws_url, std::string rest_url,
                                         int snapshot_limit, int update_speed_ms, bool book_ticker, FeedOptions options)
    : SnapshotFeed("binance", &book, stream_url(symbol, ws_url, update_speed_ms, book_ticker), std::move(options)),
      symbol(upper(symbol)),
      rest_url(rstrip(rest_url, '/')),
      snapshot_limit(snapshot_limit) {}

void BinanceSpotBookFeed::on_message(const Json& msg, int64_t recv_ts_us, Link* link) {
    if (!msg.is_object()) return;
    const Json* data = jget(msg, "data");
    const Json& event = data != nullptr ? *data : msg;  // unwrap combined-stream envelopes
    auto stream = jstr(msg, "stream");
    if (stream && ends_with(*stream, "@bookTicker")) {
        on_book_ticker(event, link != nullptr ? *link : links[0]);
        return;
    }
    auto kind = jstr(event, "e");
    if (!kind || *kind != "depthUpdate") return;
    receive(event, link);
}

void BinanceSpotBookFeed::on_book_ticker(const Json& ticker, Link& link) {
    // Kept even while the depth book resyncs: once it is seeded, a ticker
    // newer than the seeded update id is still the freshest top of book.
    // With several links only the first copy of a ticker is newer.
    const auto& t = ticker.as_object();
    copy(&link, to_int(t.at("u")) * 2 + 1);  // odd: a ticker's id, apart from the depth updates' (even)
    if (book->apply_bbo(to_float(t.at("b")), to_float(t.at("B")), to_float(t.at("a")), to_float(t.at("A")), to_int(t.at("u")))) {
        ++link.wins;
        notify();
    }
}

std::pair<int64_t, int64_t> BinanceSpotBookFeed::ids(const Json& event) const {
    const auto& e = event.as_object();
    return {to_int(e.at("U")), to_int(e.at("u"))};
}

bool BinanceSpotBookFeed::apply(const Json& event) {
    // the event that continues the book (U <= id + 1 <= u)
    const auto& e = event.as_object();
    book->apply_update(e.at("b"), e.at("a"), to_int(e.at("u")), to_int(e.at("E")) * 1'000);
    notify();
    return true;
}

void BinanceSpotBookFeed::try_sync() {
    if (!snapshot_ || buffer_.empty()) return;
    const Json snapshot = *snapshot_;
    int64_t last_update_id = to_int(snapshot.as_object().at("lastUpdateId"));
    int64_t first = first_buffered();
    if (last_update_id + 1 < first) {
        log.info("snapshot {} older than first buffered event {}; refetching", last_update_id, first);
        snapshot_.reset();
        request_snapshot(snapshot_refetch_delay);
        return;
    }
    auto pending = buffered_after(last_update_id);
    if (pending.empty()) return;  // snapshot is ahead of the stream; wait for newer events
    buffer_.clear();
    snapshot_.reset();
    book->apply_snapshot(snapshot.as_object().at("bids"), snapshot.as_object().at("asks"), last_update_id);
    if (!replay(pending)) return;
    log.info("book synced at update id {} ({} buffered events replayed)", book->seq, pending.size());
}

awaitable<Json> BinanceSpotBookFeed::fetch_snapshot() {
    std::string url = with_params(rest_url + "/api/v3/depth", {{"symbol", symbol}, {"limit", std::to_string(snapshot_limit)}});
    HttpResponse resp = co_await http_get(url, {}, 10.0);
    if (resp.status == 418 || resp.status == 429) {
        double retry_after = 5.0;
        if (auto it = resp.headers.find("retry-after"); it != resp.headers.end()) retry_after = std::stod(it->second);
        log.error("rate limited by Binance (HTTP {})", resp.status);
        co_await sleep(retry_after);
    }
    resp.raise_for_status(url);
    Json data = parse_json(resp.body);
    const auto& obj = data.as_object();
    // what the sync reads, checked here so a malformed answer is retried as a failed request
    (void)to_int(obj.at("lastUpdateId"));
    (void)obj.at("bids").as_array();
    (void)obj.at("asks").as_array();
    co_return data;
}

}  // namespace nmm
