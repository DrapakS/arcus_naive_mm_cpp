// Binance spot trades: <symbol>@aggTrade, pushed in real time.
//
// One message per price a taker order filled at (the trades of one order at one
// price aggregated), with the trade's time `T`. A sweep prints its trades before
// the book tickers settle, which is what the sweep guard wants it for: overnight
// 14-15 Oct the takers that hit our quotes did so some 80 ms after such trades,
// while our book-based guards saw the move 50-80 ms after them. (On USDⓈ-M
// futures aggTrade comes in 100 ms batches: there bookTicker is the faster
// stream, and this feed is spot only.)
//
// Aggregate trade ids only grow, so with several connections the first copy of
// each trade is passed on and later copies are dropped. The feed keeps no book:
// each new trade goes to on_trade(price, trade_us, received_us), and on_reset()
// is called when every connection is down (the trades in between are lost).
// `delays` collects how late each trade was read, from `T`.
#pragma once

#include "feeds/base.hpp"
#include "health.hpp"

namespace nmm {

using TradeListener = std::function<void(double, int64_t, int64_t)>;

class BinanceTradesFeed : public BookFeed {
public:
    BinanceTradesFeed(std::string symbol = "BTCUSDT", std::string ws_url = "wss://stream.binance.com:9443",
                      TradeListener on_trade = {}, std::function<void()> on_reset = {}, FeedOptions options = {});

    void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) override;
    void on_disconnected(Link* link) override;
    // The status line: trades passed on, the newest one's delay, the connections.
    std::string status() const;

    std::string symbol;
    TradeListener on_trade;
    std::function<void()> on_reset;
    Delays delays;  // ms from the trade's time to our reading it
    int64_t trade_count = 0;
    std::optional<int64_t> last_trade_us;  // the newest trade's time, and when we read it
    std::optional<int64_t> last_received_us;
    int64_t last_id = -1;  // aggregate trade id of the newest trade passed on
};

}  // namespace nmm
