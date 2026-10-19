// OKX V5 best bid/ask: bbo-tbt of an instrument on the public stream (the perpetual
// <ASSET>-USDT-SWAP for the guard).
//
// The top of the book, pushed every 10 ms while it changes, as a one-level snapshot
// tagged with `seqId` (the instrument's sequence, the same on every connection)
// and the exchange time `ts` (ms). With several connections the first copy of
// each snapshot wins: a message is applied only when it is newer than the book by
// (ts, seqId), so a lagging link never takes the book back and the ids
// starting again lower after an OKX service upgrade are still newer by their time.
// Sizes are in contracts (0.1 ETH, 0.01 BTC); the guard reads prices only. OKX
// closes a connection silent for 30 s and answers a text "ping" with a bare
// "pong"; the feed sends one on each connection every 20 s.
#pragma once

#include <map>

#include "feeds/base.hpp"

namespace nmm {

inline constexpr double OKX_PING_INTERVAL_S = 20.0;

class OkxTickerFeed : public BookFeed {
public:
    OkxTickerFeed(OrderBook& book, std::string inst_id = "BTC-USDT-SWAP", std::string ws_url = "wss://ws.okx.com:8443",
                  FeedOptions options = {}, double ping_interval = OKX_PING_INTERVAL_S);
    ~OkxTickerFeed() override;

    void on_connected(Link& link) override;
    bool skips(std::string_view text) const override { return text == "pong"; }
    bool carries_data(const Json& msg) const override;
    void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) override;
    void on_disconnected(Link* link) override;

    std::string inst_id;
    Json arg;  // {"channel": "bbo-tbt", "instId": inst_id}
    double ping_interval;
    std::pair<int64_t, int64_t> last {0, 0};  // (ts, seqId) of the newest snapshot applied

private:
    awaitable<void> ping(Link& link, std::shared_ptr<WsConnection> ws);
    std::map<int, Task> pingers_;
};

}  // namespace nmm
