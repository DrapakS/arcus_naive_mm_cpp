#include "feeds/bybit.hpp"

#include <stdexcept>

#include "util/pyfmt.hpp"

namespace nmm {

namespace {

std::string checked_category(std::string category) {
    if (category != "linear" && category != "spot") throw std::invalid_argument("bybit category must be one of linear, spot");
    return category;
}

std::string stream_url(const std::string& category, const std::string& ws_url) {
    std::string_view base = remove_suffix(rstrip(ws_url, '/'), "/v5/public/" + category);
    return std::string(base) + "/v5/public/" + category;
}

}  // namespace

BybitTickerFeed::BybitTickerFeed(OrderBook& book, std::string category, std::string symbol, std::string ws_url,
                                 FeedOptions options, double ping_interval)
    : BookFeed("bybit-" + checked_category(category), &book, stream_url(category, ws_url), std::move(options)),
      category(category),
      symbol(upper(symbol)),
      topic("orderbook.1." + upper(symbol)),
      ping_interval(ping_interval) {}

BybitTickerFeed::~BybitTickerFeed() = default;

void BybitTickerFeed::on_connected(Link& link) {
    send_json(JsonObject {{"op", "subscribe"}, {"args", JsonArray {Json(topic)}}}, &link);
    if (auto old = pingers_.find(link.index); old != pingers_.end()) {
        old->second.cancel();
        pingers_.erase(old);
    }
    pingers_[link.index] = Task::spawn(ping(link, link.ws), name + "-ping", "naive_mm." + name);
}

awaitable<void> BybitTickerFeed::ping(Link& link, std::shared_ptr<WsConnection> ws) {
    for (;;) {
        co_await sleep(ping_interval);
        if (link.ws != ws) co_return;  // that connection is gone
        ws->send(R"({"op":"ping"})");
    }
}

bool BybitTickerFeed::carries_data(const Json& msg) const {
    auto t = jstr(msg, "topic");
    return t && *t == topic;  // not our pongs' answers
}

void BybitTickerFeed::on_message(const Json& msg, int64_t recv_ts_us, Link* link) {
    auto t = jstr(msg, "topic");
    if (!msg.is_object() || !t || *t != topic) {
        if (msg.is_object()) {
            auto op = jstr(msg, "op");
            const Json* success = jget(msg, "success");
            if (op && *op == "subscribe" && success != nullptr && success->is_bool() && !success->get_bool()) {
                // a symbol this category does not list: the link stays silent, say why
                log.warning("subscribing to {} refused: {}", topic, jstr_or(msg, "ret_msg"));
            }
        }
        return;  // subscribe acks, pongs
    }
    const Json* data_field = jget(msg, "data");
    if (!truthy(data_field)) return;
    const Json& data = *data_field;
    const Json* bids = jget(data, "b");
    const Json* asks = jget(data, "a");
    if (!truthy(bids) || !truthy(asks)) return;  // one side empty: keep the last full top
    // no engine time: the push time, so the top still moves
    const Json* cts_field = jget(msg, "cts");
    if (!truthy(cts_field)) cts_field = jget(msg, "ts");
    int64_t cts = truthy(cts_field) ? to_int(*cts_field) : 0;
    int64_t u = to_int(data.as_object().at("u"));
    std::pair<int64_t, int64_t> key {cts, u};
    arrived(link, cts * 1'000'003 + u, recv_ts_us);  // (cts, u)
    if (key <= last) return;  // another link delivered it first, or it is older than the book
    last = key;
    ++(link != nullptr ? *link : links[0]).wins;
    const auto& bid = bids->as_array().at(0).as_array();
    const auto& ask = asks->as_array().at(0).as_array();
    book->apply_snapshot(std::vector<Level> {{to_float(bid.at(0)), to_float(bid.at(1))}},
                         std::vector<Level> {{to_float(ask.at(0)), to_float(ask.at(1))}}, u,
                         cts ? std::optional<int64_t>(cts * 1'000) : std::nullopt);
    notify();
}

void BybitTickerFeed::on_disconnected(Link* link) {
    if (link != nullptr) {
        if (auto task = pingers_.find(link->index); task != pingers_.end()) {
            task->second.cancel();
            pingers_.erase(task);
        }
    }
    if (link != nullptr && others_up(*link)) return;  // the other links keep the top current
    last = {0, 0};
    BookFeed::on_disconnected(link);
}

}  // namespace nmm
