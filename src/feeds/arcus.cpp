#include "feeds/arcus.hpp"

#include <algorithm>
#include <stdexcept>

#include "util/pyfmt.hpp"

namespace nmm {

ArcusBookFeed::ArcusBookFeed(OrderBook& book, std::string market, std::string ws_url, std::string rest_url, int n_levels,
                             int min_levels, FeedOptions options)
    : SnapshotFeed("arcus", &book, std::move(ws_url), std::move(options)),
      market(std::move(market)),
      rest_url(rstrip(rest_url, '/')),
      n_levels(n_levels),
      min_levels(min_levels) {
    if (n_levels < 1 || n_levels > 100) throw std::invalid_argument("Arcus n_levels must be within 1..100");
    if (min_levels < 1 || min_levels > n_levels) throw std::invalid_argument("Arcus min_levels must be within 1..n_levels");
    snapshot_refetch_delay = 0.1;
}

void ArcusBookFeed::on_connected(Link& link) {
    SnapshotFeed::on_connected(link);
    send_json(JsonObject {{"type", "subscribe"}, {"channel", ARCUS_BOOK_CHANNEL}, {"id", market}, {"snapshot", false}}, &link);
}

bool ArcusBookFeed::carries_data(const Json& msg) const {
    auto type = jstr(msg, "type");
    auto channel = jstr(msg, "channel");
    return type && *type == "channel_data" && channel && *channel == ARCUS_BOOK_CHANNEL;
}

void ArcusBookFeed::on_message(const Json& msg, int64_t recv_ts_us, Link* link) {
    if (!msg.is_object()) return;
    const Json* kind_field = jget(msg, "type");
    std::string kind = kind_field != nullptr ? to_str(*kind_field) : "None";
    const Json* channel = jget(msg, "channel");
    if (channel == nullptr || !channel->is_string() || channel->get_string() != ARCUS_BOOK_CHANNEL) {
        if (kind == "connected") log.debug("connection id {}", jstr_or(msg, "connection_id"));
        else if (kind != "subscribed" && kind != "unsubscribed" && kind != "channel_data")
            log.warning("unexpected message: {}", dump_json(msg));
        return;
    }
    const Json* id = jget(msg, "id");
    if (upper(id != nullptr ? to_str(*id) : market) != upper(market)) return;
    if (kind == "channel_data") {
        const Json* delta = jget(msg, "contents");
        if (truthy(delta)) receive(*delta, link);
    } else if (kind != "subscribed" && kind != "unsubscribed") {
        log.warning("unexpected {} message: {}", ARCUS_BOOK_CHANNEL, dump_json(msg));
    }
}

std::pair<int64_t, int64_t> ArcusBookFeed::ids(const Json& delta) const {
    int64_t seq = to_int(delta.as_object().at("lastSequenceId"));
    return {seq, seq};
}

bool ArcusBookFeed::apply(const Json& delta) {
    int64_t seq = to_int(delta.as_object().at("lastSequenceId"));
    std::vector<Level> bids, asks;
    if (const Json* b = jget(delta, "bids"); truthy(b)) {
        for (const auto& level : b->as_array()) {
            double price = to_float(level.as_array().at(0));
            if (!bid_floor || price >= *bid_floor) bids.emplace_back(price, to_float(level.as_array().at(1)));
        }
    }
    if (const Json* a = jget(delta, "asks"); truthy(a)) {
        for (const auto& level : a->as_array()) {
            double price = to_float(level.as_array().at(0));
            if (!ask_ceiling || price <= *ask_ceiling) asks.emplace_back(price, to_float(level.as_array().at(1)));
        }
    }
    book->apply_update(bids, asks, seq);
    auto [n_bids, n_asks] = book->depth();
    if ((bid_floor && n_bids < static_cast<size_t>(min_levels)) || (ask_ceiling && n_asks < static_cast<size_t>(min_levels))) {
        log.info("fewer than {} known levels on a side ({} bids, {} asks); refreshing snapshot", min_levels, n_bids, n_asks);
        ++refresh_count;
        resync();
        return false;
    }
    notify();
    return true;
}

void ArcusBookFeed::try_sync() {
    if (!snapshot_ || buffer_.empty()) return;
    const Json snapshot = *snapshot_;
    int64_t seq = to_int(snapshot.as_object().at("lastSequenceId"));
    int64_t first = first_buffered();
    if (seq + 1 < first) {
        log.debug("snapshot {} older than first buffered delta {}; refetching", seq, first);
        snapshot_.reset();
        request_snapshot(snapshot_refetch_delay);
        return;
    }
    auto pending = buffered_after(seq);
    if (pending.empty()) return;  // snapshot is ahead of the stream; wait for newer deltas
    buffer_.clear();
    snapshot_.reset();
    const Json* b = jget(snapshot, "bids");
    const Json* a = jget(snapshot, "asks");
    Json bids = truthy(b) ? *b : Json(JsonArray {});
    Json asks = truthy(a) ? *a : Json(JsonArray {});
    const Json* ts = jget(snapshot, "timestamp");
    std::optional<int64_t> stamp;
    if (ts != nullptr && !ts->is_null()) stamp = to_int(*ts);
    book->apply_snapshot(bids, asks, seq, stamp);
    auto prices = [](const Json& side) {
        std::vector<double> out;
        for (const auto& level : side.as_array()) out.push_back(to_float(level.as_array().at(0)));
        return out;
    };
    auto bid_prices = prices(bids), ask_prices = prices(asks);
    bid_floor = bid_prices.size() >= static_cast<size_t>(n_levels) ? std::optional(*std::min_element(bid_prices.begin(), bid_prices.end()))
                                                                    : std::nullopt;
    ask_ceiling = ask_prices.size() >= static_cast<size_t>(n_levels) ? std::optional(*std::max_element(ask_prices.begin(), ask_prices.end()))
                                                                      : std::nullopt;
    if (!replay(pending)) return;
    auto [n_bids, n_asks] = book->depth();
    log.info("book synced at seq {} ({} bids, {} asks; {} buffered deltas replayed)", book->seq, n_bids, n_asks, pending.size());
}

awaitable<Json> ArcusBookFeed::fetch_snapshot() {
    std::string url = with_params(rest_url + "/v1/l2OrderBook/" + market, {{"nLevels", std::to_string(n_levels)}});
    HttpResponse resp = co_await http_get(url, {}, 10.0);
    resp.raise_for_status(url);
    Json data = parse_json(resp.body);
    if (!data.is_object() || !data.as_object().contains("lastSequenceId")) {
        // an error body with status 200: no book to sync to, so a failed request (retried)
        throw std::invalid_argument("snapshot without lastSequenceId: " + to_str(data).substr(0, 200));
    }
    (void)to_int(data.as_object().at("lastSequenceId"));  // what the sync reads: a malformed answer is a failed request
    co_return data;
}

}  // namespace nmm
