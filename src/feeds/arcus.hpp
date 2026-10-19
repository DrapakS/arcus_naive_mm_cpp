// Arcus perps order book feed: l2OrderbookUpdates deltas over a REST snapshot.
//
// Protocol (docs.arcus.xyz, "WebSocket" and "L2 orderbook updates"):
//
// * One socket multiplexes all channels; the server first sends
//   {"type": "connected"}. channel_data frames carry deltas with absolute level
//   sizes (0 = remove), applied in array order, and a per-market lastSequenceId
//   that is contiguous (+1 per delta). Deltas carry no timestamp.
//   globalSequenceId is cross-market and is not gap-checked.
// * The snapshot that comes with a plain subscribe lags the delta head, and the
//   deltas in between are never sent. The docs call that gap harmless, but live
//   it leaves stale levels that a later delta may never touch, enough to cross
//   the book. So we subscribe with "snapshot": false and follow the same
//   procedure as for Binance: buffer deltas, fetch GET /v1/l2OrderBook (it
//   carries lastSequenceId), refetch while it is older than the first buffered
//   delta, then seed from it and replay the newer deltas. A gap mid-stream
//   rebuilds the same way, without resubscribing.
// * nLevels only limits the snapshot: deltas cover every price. Past the
//   snapshot's worst level the book is unknown, so deltas there are ignored, and
//   when fewer than `min_levels` known levels remain on a side (the market moved
//   through them) the book is rebuilt from a new snapshot. A snapshot side
//   shorter than nLevels is the whole side and has no such limit.
#pragma once

#include "feeds/base.hpp"

namespace nmm {

inline constexpr const char* ARCUS_BOOK_CHANNEL = "l2OrderbookUpdates";

class ArcusBookFeed : public SnapshotFeed {
public:
    ArcusBookFeed(OrderBook& book, std::string market = "BTC-USD", std::string ws_url = "wss://api.arcus.xyz/v1/ws",
                  std::string rest_url = "https://api.arcus.xyz", int n_levels = 100, int min_levels = 10,
                  FeedOptions options = {});

    void on_connected(Link& link) override;
    bool carries_data(const Json& msg) const override;
    void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) override;
    awaitable<Json> fetch_snapshot() override;

    std::string market;
    std::string rest_url;
    int n_levels;
    int min_levels;
    // Worst price each side of the book is known down/up to; none = the whole side.
    std::optional<double> bid_floor;
    std::optional<double> ask_ceiling;

protected:
    std::pair<int64_t, int64_t> ids(const Json& delta) const override;
    bool apply(const Json& delta) override;
    void try_sync() override;
};

}  // namespace nmm
