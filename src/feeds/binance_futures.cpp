#include "feeds/binance_futures.hpp"

#include "util/pyfmt.hpp"

namespace nmm {

namespace {

std::string stream_url(const std::string& symbol, const std::string& ws_url) {
    std::string_view base = remove_suffix(remove_suffix(remove_suffix(rstrip(ws_url, '/'), "/ws"), "/stream"), "/public");
    return std::string(base) + "/public/stream?streams=" + lower(symbol) + "@bookTicker";
}

}  // namespace

BinanceFuturesTickerFeed::BinanceFuturesTickerFeed(OrderBook& book, std::string symbol, std::string ws_url, FeedOptions options)
    : BookFeed("binance-futures", &book, stream_url(symbol, ws_url), std::move(options)), symbol(upper(symbol)) {}

void BinanceFuturesTickerFeed::on_message(const Json& msg, int64_t recv_ts_us, Link* link) {
    if (!msg.is_object()) return;
    const Json* data = jget(msg, "data");
    const Json& ticker = data != nullptr ? *data : msg;  // unwrap the combined-stream envelope
    if (!ticker.is_object()) return;
    const auto& t = ticker.as_object();
    if (!t.contains("u") || !t.contains("b")) return;  // subscription replies and the like
    int64_t update_id = to_int(t.at("u"));
    arrived(link, update_id, recv_ts_us);
    if (update_id <= last_id) return;  // another link delivered it first
    last_id = update_id;
    ++(link != nullptr ? *link : links[0]).wins;
    std::optional<int64_t> event_us;
    if (const Json* e = t.if_contains("E")) event_us = to_int(*e) * 1'000;
    if (event_us) delays.add(static_cast<double>(recv_ts_us - *event_us) / 1'000);
    book->apply_snapshot(std::vector<Level> {{to_float(t.at("b")), to_float(t.at("B"))}},
                         std::vector<Level> {{to_float(t.at("a")), to_float(t.at("A"))}}, update_id, event_us);
    notify();
}

void BinanceFuturesTickerFeed::on_disconnected(Link* link) {
    if (link != nullptr && others_up(*link)) return;  // the other links keep the top current
    last_id = 0;
    BookFeed::on_disconnected(link);
}

}  // namespace nmm
