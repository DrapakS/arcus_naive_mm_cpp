// Binance USDⓈ-M futures best bid/ask: <symbol>@bookTicker, pushed in real time.
//
// Only the top of the book is kept: every ticker replaces it (a one-level
// snapshot), tagged with the ticker's order book update id `u` and event time
// `E`. Update ids only grow, so with several connections the first copy of each
// ticker wins and later copies are dropped. `delays` collects how late each
// applied ticker was read, from `E`. The futures market tends to move before
// spot, which is what the price guard wants it for.
//
// Since 2026 Binance routes futures streams by path: the book streams
// (bookTicker, depth) are served under /public, trades and mark prices under
// /market. The feed connects to /public; a ws_url given with or without that
// path works the same.
#pragma once

#include "feeds/base.hpp"
#include "health.hpp"

namespace nmm {

class BinanceFuturesTickerFeed : public BookFeed {
public:
    BinanceFuturesTickerFeed(OrderBook& book, std::string symbol = "BTCUSDT", std::string ws_url = "wss://fstream.binance.com",
                             FeedOptions options = {});

    void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) override;
    void on_disconnected(Link* link) override;

    std::string symbol;
    Delays delays;  // ms from each applied ticker's event time to our reading it
    int64_t last_id = 0;  // update id of the newest ticker applied
};

}  // namespace nmm
