// The buy and sell prices the bot quotes, one stdout line whenever either changes.
//
// On every market update the buy and sell prices are worked out exactly as the
// strategy does (same formulas, ticks and rounding): with both sides quoted, they
// are the very prices the strategy loop just worked out for the update.
//
// Mode quote prints them without trading. A trading bot leaves its order alone
// until the target is more than tol away; "re-place" marks the updates where it
// would move it, and the status line counts them. While a guard pauses a side
// ("drop pause": no buying, "rise pause": no selling) the order a trading bot
// would have on that side is pulled; while the lag guard pauses both, "late data
// pause". The trading modes print the same lines, ending with the working order:
// its price and state, how far it is from its target, how deep it sits behind the
// best price on its side of the Arcus book, and "alone on top" when nobody else is
// at its price or better.
//
// Each line also goes to logger naive_mm.quotes when it has a handler
// (--orders-log adds one). [agent] quote_line_ms writes at most one line that
// often; 0 = one on every change.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "trading/strategy.hpp"

namespace nmm {

class QuoteWatch {
public:
    explicit QuoteWatch(QuoteStrategy& strategy, double every_s = 0.0);

    void update(const MarketView& view);
    std::string status() const;

    QuoteStrategy& strategy;
    std::map<std::string, std::optional<Decimal>> targets {{"BUY", std::nullopt}, {"SELL", std::nullopt}};
    std::map<std::string, std::optional<Decimal>> resting {{"BUY", std::nullopt}, {"SELL", std::nullopt}};  // where the orders would stand
    std::map<std::string, int64_t> moves {{"BUY", 0}, {"SELL", 0}};  // placements a trading bot would have made
    double every_s;
    Clock clock = monotonic;
    Logger* log;

private:
    std::vector<std::string> moves_for(const std::map<std::string, std::optional<Decimal>>& targets_now);
    std::string order_text(const MarketView& view);
    std::string one(const Order& order, const MarketView& view);
    std::string line(const MarketView& view, const std::string& tail);

    std::map<std::string, std::optional<Decimal>> shown_ {{"BUY", std::nullopt}, {"SELL", std::nullopt}};  // the prices the last line showed
    double last_line_ = -1e300;
    std::vector<std::string> moved_;  // mode quote: the re-places since the last line
};

}  // namespace nmm
