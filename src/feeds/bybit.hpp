// Bybit V5 best bid/ask: orderbook.1.<symbol> on the public spot or linear stream.
//
// Level 1 comes as snapshots only, pushed every 10 ms while the top changes, and
// again with the same update id `u` after 3 s without a change. Every message
// replaces the top of the book (a one-level snapshot) tagged with `u` and the
// matching engine time `cts`. With several connections the first copy of each
// snapshot wins: a message is applied only when it is newer than the book by
// (cts, u), so a lagging link never takes the book back and the update id
// restarting from 1 after a Bybit service restart is still newer by its engine
// time. Bybit wants an application {"op": "ping"} about every 20 s; the feed
// sends one on each connection.
#pragma once

#include <map>

#include "feeds/base.hpp"

namespace nmm {

inline constexpr double BYBIT_PING_INTERVAL_S = 20.0;

class BybitTickerFeed : public BookFeed {
public:
    BybitTickerFeed(OrderBook& book, std::string category = "linear", std::string symbol = "BTCUSDT",
                    std::string ws_url = "wss://stream.bybit.com", FeedOptions options = {},
                    double ping_interval = BYBIT_PING_INTERVAL_S);
    ~BybitTickerFeed() override;

    void on_connected(Link& link) override;
    bool carries_data(const Json& msg) const override;
    void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) override;
    void on_disconnected(Link* link) override;

    std::string category;
    std::string symbol;
    std::string topic;
    double ping_interval;
    std::pair<int64_t, int64_t> last {0, 0};  // (cts, u) of the newest snapshot applied

private:
    awaitable<void> ping(Link& link, std::shared_ptr<WsConnection> ws);
    std::map<int, Task> pingers_;
};

}  // namespace nmm
