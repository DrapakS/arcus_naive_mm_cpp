// Binance spot book feed: diff depth synced to a REST snapshot, plus bookTicker.
//
// The depth book follows Binance's "how to manage a local order book correctly"
// procedure:
//
// 1. Open <symbol>@depth@100ms and buffer events.
// 2. Fetch GET /api/v3/depth once the first event has arrived.
// 3. If the snapshot is older than the first buffered event, fetch it again.
// 4. Drop buffered events with u <= lastUpdateId, seed the book with the
//    snapshot, then replay the rest.
// 5. For each later event: ignore u <= local id; if U > local id + 1 an event
//    was missed, so throw the book away and start over from 1.
//
// Diff depth arrives in 100ms batches. <symbol>@bookTicker pushes the best
// bid/ask in real time, tagged with the same order book update id (u), and
// comes over the same connection as a combined stream. Each ticker is layered
// over the depth book with OrderBook::apply_bbo, so the top of book stays
// current between batches while the depth book and its sequence checks stay
// exactly as Binance's procedure defines them.
#pragma once

#include "feeds/base.hpp"

namespace nmm {

class BinanceSpotBookFeed : public SnapshotFeed {
public:
    BinanceSpotBookFeed(OrderBook& book, std::string symbol = "BTCUSDT", std::string ws_url = "wss://stream.binance.com:9443",
                        std::string rest_url = "https://api.binance.com", int snapshot_limit = 1000,
                        int update_speed_ms = 100, bool book_ticker = true, FeedOptions options = {});

    void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) override;
    // The snapshot as Binance sends it: {"lastUpdateId": .., "bids": [..], "asks": [..]}.
    awaitable<Json> fetch_snapshot() override;

    std::string symbol;
    std::string rest_url;
    int snapshot_limit;

protected:
    std::pair<int64_t, int64_t> ids(const Json& event) const override;
    bool apply(const Json& event) override;
    void try_sync() override;

private:
    void on_book_ticker(const Json& ticker, Link& link);
};

}  // namespace nmm
