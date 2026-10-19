#include "feeds/okx.hpp"

#include "util/pyfmt.hpp"

namespace nmm {

namespace {

std::string stream_url(const std::string& ws_url) {
    return std::string(remove_suffix(rstrip(ws_url, '/'), "/ws/v5/public")) + "/ws/v5/public";
}

}  // namespace

OkxTickerFeed::OkxTickerFeed(OrderBook& book, std::string inst_id, std::string ws_url, FeedOptions options,
                             double ping_interval)
    : BookFeed("okx", &book, stream_url(ws_url), std::move(options)),
      inst_id(upper(inst_id)),
      arg(JsonObject {{"channel", "bbo-tbt"}, {"instId", upper(inst_id)}}),
      ping_interval(ping_interval) {}

OkxTickerFeed::~OkxTickerFeed() = default;

void OkxTickerFeed::on_connected(Link& link) {
    send_json(JsonObject {{"op", "subscribe"}, {"args", JsonArray {arg}}}, &link);
    if (auto old = pingers_.find(link.index); old != pingers_.end()) {
        old->second.cancel();
        pingers_.erase(old);
    }
    pingers_[link.index] = Task::spawn(ping(link, link.ws), name + "-ping", "naive_mm." + name);
}

awaitable<void> OkxTickerFeed::ping(Link& link, std::shared_ptr<WsConnection> ws) {
    for (;;) {
        co_await sleep(ping_interval);
        if (link.ws != ws) co_return;  // that connection is gone
        ws->send("ping");
    }
}

bool OkxTickerFeed::carries_data(const Json& msg) const {
    return msg.is_object() && msg.as_object().contains("data");  // not the subscribe ack
}

void OkxTickerFeed::on_message(const Json& msg, int64_t recv_ts_us, Link* link) {
    if (!msg.is_object()) return;
    if (jstr(msg, "event") == "error") {
        // an instrument OKX does not list: the link stays silent, say why
        log.warning("subscribing to {} refused: {}", inst_id, jstr_or(msg, "msg"));
        return;
    }
    const Json* data = jget(msg, "data");
    const Json* inst = jget(jget(msg, "arg"), "instId");
    if (!truthy(data) || inst == nullptr || !inst->is_string() || inst->get_string() != inst_id) return;  // acks, notices
    const Json& top = data->as_array().at(0);
    const Json* bids = jget(top, "bids");
    const Json* asks = jget(top, "asks");
    if (!truthy(bids) || !truthy(asks)) return;  // one side empty: keep the last full top
    const Json* ts_field = jget(top, "ts");
    int64_t ts = truthy(ts_field) ? to_int(*ts_field) : 0;
    int64_t seq = to_int(top.as_object().at("seqId"));
    std::pair<int64_t, int64_t> key {ts, seq};
    copy(link, ts * 1'000'003 + seq);  // how far behind the other links its link runs
    if (key <= last) return;  // another link delivered it first, or it is older than the book
    last = key;
    ++(link != nullptr ? *link : links[0]).wins;
    const auto& bid = bids->as_array().at(0).as_array();
    const auto& ask = asks->as_array().at(0).as_array();
    book->apply_snapshot(std::vector<Level> {{to_float(bid.at(0)), to_float(bid.at(1))}},
                         std::vector<Level> {{to_float(ask.at(0)), to_float(ask.at(1))}}, seq,
                         ts ? std::optional<int64_t>(ts * 1'000) : std::nullopt);
    notify();
}

void OkxTickerFeed::on_disconnected(Link* link) {
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
