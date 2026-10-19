// What the strategy sees on every market data change.
#pragma once

#include <memory>
#include <optional>

#include "orderbook.hpp"

namespace nmm {

// What the strategy sees. A side is null while its book is not ready.
struct MarketView {
    BookTopPtr reference;  // Binance spot, the price source
    BookTopPtr venue;  // Arcus perp, where we quote
    std::optional<double> fair_bid;  // estimated Arcus best bid; none until it can be priced
    BookTopPtr futures;  // Binance futures best bid/ask, when that feed is on and ready
    std::optional<double> futures_fair_bid;  // the fair bid priced off futures (for the logs when pricing off spot)
    std::optional<double> spot_fair_bid;  // the fair bid priced off spot (for the logs when pricing off futures)
    BookTopPtr bybit;  // Bybit best bid/ask, when that feed is on and ready
    // (futures fair / spot fair - 1) in bp now, and its change over the last GAP_WINDOW_S: how far
    // futures (which move first) have pulled away from spot; for the logs
    std::optional<double> fair_gap_bps;
    std::optional<double> fair_gap_change_bps;
    // pricing.model = "ml": the model's prediction (P_next / P_prev - 1) in bp; for the logs
    std::optional<double> model_bps;

    std::optional<double> basis_bps() const {
        if (reference == nullptr || venue == nullptr) return std::nullopt;
        auto ref_mid = reference->mid();
        auto venue_mid = venue->mid();
        if (!ref_mid || *ref_mid == 0.0 || !venue_mid) return std::nullopt;
        return (*venue_mid / *ref_mid - 1.0) * 1e4;
    }
};
using MarketViewPtr = std::shared_ptr<const MarketView>;

}  // namespace nmm
