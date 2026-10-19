#include "feeds/arcus_trades.hpp"

#include <functional>

namespace nmm {

ArcusTradesFeed::ArcusTradesFeed(std::string market_, std::string ws_url, std::function<void(const Json&)> on_trades_,
                                 std::function<void()> on_up_, std::function<void()> on_down_, FeedOptions options)
    : BookFeed("arcus-trades", nullptr, std::move(ws_url), std::move(options)),
      market(std::move(market_)),
      on_trades(std::move(on_trades_)),
      on_up(std::move(on_up_)),
      on_down(std::move(on_down_)) {}

void ArcusTradesFeed::on_connected(Link& link) {
    send_json(JsonObject {{"type", "subscribe"}, {"channel", "trades"}, {"id", market}}, &link);
    if (!up_) {
        up_ = true;
        if (on_up) on_up();
    }
}

bool ArcusTradesFeed::carries_data(const Json& msg) const {
    return jstr(msg, "channel") == "trades" && jstr(msg, "type") == "channel_data";
}

void ArcusTradesFeed::on_message(const Json& msg, int64_t recv_ts_us, Link* link) {
    if (!carries_data(msg)) return;  // the greeting, subscribe acks
    const Json* contents = jget(msg, "contents");
    if (contents == nullptr || !contents->is_array() || contents->get_array().empty()) return;
    const Json& first = contents->get_array()[0];
    const Json* trade_id = jget(first, "tradeId");
    std::string key = truthy(trade_id) ? to_str(*trade_id) : jstr_or(first, "timestamp") + "/" + jstr_or(first, "size");
    if (!copy(link, static_cast<int64_t>(std::hash<std::string> {}(key)))) return;  // another link delivered it first
    ++(link != nullptr ? *link : links[0]).wins;
    ++frame_count;
    if (on_trades) on_trades(msg);
}

void ArcusTradesFeed::on_disconnected(Link* link) {
    if (link != nullptr && others_up(*link)) return;  // the other links carry the trades
    if (up_) {
        up_ = false;
        if (on_down) on_down();
    }
}

}  // namespace nmm
