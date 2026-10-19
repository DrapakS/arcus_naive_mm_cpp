// Arcus public trades of a market (channel "trades"), over several connections.
//
// A frame holds the trades of one taker order. Each frame is passed on once, from the
// link that delivers it first (by the tradeId of its first trade; a trade without one by
// its time and size), and the later copies measure how far behind their links run: a
// connection Arcus feeds late stays late (19 Oct: 73 ms behind for a whole run), and with
// slow_link_ms set it is reconnected. on_up is called when a link connects while none was
// up, on_down when the last one goes (the trades in between are lost).
#pragma once

#include "feeds/base.hpp"

namespace nmm {

class ArcusTradesFeed : public BookFeed {
public:
    ArcusTradesFeed(std::string market, std::string ws_url, std::function<void(const Json&)> on_trades,
                    std::function<void()> on_up = {}, std::function<void()> on_down = {}, FeedOptions options = {});

    void on_connected(Link& link) override;
    bool carries_data(const Json& msg) const override;
    void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) override;
    void on_disconnected(Link* link) override;

    std::string market;
    std::function<void(const Json&)> on_trades;  // the whole channel_data frame
    std::function<void()> on_up, on_down;
    int64_t frame_count = 0;  // frames passed on

private:
    bool up_ = false;
};

}  // namespace nmm
