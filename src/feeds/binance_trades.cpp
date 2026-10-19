#include "feeds/binance_trades.hpp"

#include <format>
#include <numeric>

#include "util/pyfmt.hpp"

namespace nmm {

namespace {

std::string stream_url(const std::string& symbol, const std::string& ws_url) {
    std::string_view base = remove_suffix(remove_suffix(rstrip(ws_url, '/'), "/ws"), "/stream");
    return std::string(base) + "/stream?streams=" + lower(symbol) + "@aggTrade";
}

}  // namespace

BinanceTradesFeed::BinanceTradesFeed(std::string symbol, std::string ws_url, TradeListener on_trade,
                                     std::function<void()> on_reset, FeedOptions options)
    : BookFeed("binance-trades", nullptr, stream_url(symbol, ws_url), std::move(options)),
      symbol(upper(symbol)),
      on_trade(std::move(on_trade)),
      on_reset(std::move(on_reset)) {}

void BinanceTradesFeed::on_message(const Json& msg, int64_t recv_ts_us, Link* link) {
    if (!msg.is_object()) return;
    const Json* data = jget(msg, "data");
    const Json& trade = data != nullptr ? *data : msg;  // unwrap the combined-stream envelope
    auto kind = jstr(trade, "e");
    if (!kind || *kind != "aggTrade") return;
    const auto& t = trade.as_object();
    int64_t trade_id = to_int(t.at("a"));
    if (trade_id <= last_id) return;  // another link delivered it first
    last_id = trade_id;
    ++(link != nullptr ? *link : links[0]).wins;
    ++trade_count;
    int64_t trade_us = to_int(t.at("T")) * 1'000;
    last_trade_us = trade_us;
    last_received_us = recv_ts_us;
    delays.add(static_cast<double>(recv_ts_us - trade_us) / 1'000);
    if (on_trade) on_trade(to_float(t.at("p")), trade_us, recv_ts_us);
}

void BinanceTradesFeed::on_disconnected(Link* link) {
    if (link != nullptr && others_up(*link)) return;  // the other links keep the trades coming
    last_id = -1;
    if (on_reset) on_reset();
}

std::string BinanceTradesFeed::status() const {
    std::vector<std::string> parts;
    parts.push_back(std::format("{:<8} {:<8} {:<8}", "trades", symbol, links_up() ? "OK" : "DOWN"));
    if (last_trade_us) {
        parts.push_back(std::format("trades {}, last read {:.0f}ms after it", trade_count,
                                    static_cast<double>(*last_received_us - *last_trade_us) / 1'000));
    } else {
        parts.push_back("no trade yet");
    }
    parts.push_back(std::format("msgs {} conn {}", message_count, connect_count));
    if (links.size() > 1) {
        int64_t total = 0;
        for (const auto& l : links) total += l.wins;
        if (!total) total = 1;
        std::vector<std::string> shares;
        for (const auto& l : links) shares.push_back(std::to_string(l.wins * 100 / total));
        parts.push_back(std::format("links {}/{} first {}%", links_up(), links.size(), join(shares, "/")));
    }
    return join(parts, "  ");
}

}  // namespace nmm
