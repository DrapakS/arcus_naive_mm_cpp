// Inventory by asset class across the bots, one bot (one process) a market.
//
// The bots of a host share a directory: each one writes its own file there
// (<market>.<pid>.json: market, asset class, position, mark price, risk weight,
// when) and reads the others' every `interval` (100 ms). No server, no lock: a
// file is written aside and renamed over the old one, so a reader sees a whole
// file or the previous one. Our file is rewritten when the position changes and
// once a second besides (the mark, and the time that says the bot is alive);
// another's is read again only when its modification time changed, so a quiet
// round is a directory listing and a few stats. Live bots only: a paper bot's
// position is not real, and the two variants of a paper A/B would count each other.
// The Python bot writes and reads the same files: a Python bot and a C++ bot may
// share one directory.
//
// The class exposure is the sum over its markets of weight x position x mark,
// ours included as it is now; the weight puts a market in BTC units of risk (its
// 1-min volatility over BTC's: 19 Oct, ETH 1.36, SOL 1.70, both 0.86-0.91
// correlated with BTC). u = exposure / limit_usd, within -1..1. An order that
// adds to the class's lean takes markup x (1 + factor u), one that takes it back
// markup x (1 - factor u): with the class long, buys are quoted further out and
// sells nearer, in every market of the class, on top of each bot's own skew.
// Besides, with `shift_bp`, both quotes move by shift_bp x u against the lean
// (the class long: both lower, by shift_bp at u = 1): the markup is a fraction of
// a bp, so its x0.7..x1.3 is a tick or so on ETH (19 Oct: the class short 82% of
// the time, at u = -1 18%); the shift does not shrink with the markup. No taker,
// no stop-loss: only where the quotes stand.
//
// A file not rewritten for `stale_s` (its bot stopped) is left out, and said
// once; so is a file of our own market from another process (a paper A/B variant
// of the same market: an alternative, not more of the position). Two other bots
// on one market: the newer file counts.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/aio.hpp"
#include "util/clock.hpp"
#include "util/decimal.hpp"

namespace nmm {

inline const std::vector<std::string> ASSET_CLASSES {"crypto", "stocks", "metals"};
constexpr double HEARTBEAT_S = 1.0;  // our file is rewritten at least this often, the position changed or not

// Path(path).expanduser() as Python prints it: "~" for the home directory, repeated slashes and
// "." parts dropped.
std::string expand_path(std::string_view path);

struct Holding {
    std::string market;
    std::string asset_class;
    double position = 0.0;
    double mark = 0.0;
    double weight = 1.0;
    double at = 0.0;  // wall clock, when its bot wrote it
    int64_t pid = 0;

    // Its exposure in BTC units of risk, in dollars.
    double usd() const { return weight * position * mark; }
};

struct PortfolioOptions {
    double weight = 1.0;
    double limit_usd = 1500.0;
    double factor = 0.3;
    double shift_bp = 0.0;
    double stale_s = 30.0;
    double interval_s = 0.1;
    std::function<double()> position = [] { return 0.0; };
    std::function<std::optional<double>()> mark = [] { return std::optional<double>(); };
    Clock clock = wall_time;
    std::optional<int64_t> pid;  // none: this process's
};

class Portfolio {
public:
    // Throws std::invalid_argument for an asset class not in ASSET_CLASSES.
    Portfolio(std::string_view directory, std::string market, std::string asset_class, PortfolioOptions options = {});

    // ------------------------------------------------------------- the files

    // Our file, when the position changed or a second went by (or `force`): written aside, then
    // renamed over the old one. True when written. Throws std::system_error when it cannot be.
    bool publish(bool force = false);
    // The other bots' files: the fresh ones of our class, one a market (the newest).
    void read();
    // Our file goes with us: a stopped bot's position is no longer quoted against.
    void close();
    // publish() and read() every interval_s until cancelled; our file is removed then.
    awaitable<void> run();

    // --------------------------------------------------------------- pricing

    // The class's exposure with ours as it is now ($, BTC units of risk; + long).
    double exposure(double position, double price) const;
    // u: the exposure over limit_usd, within -1..1.
    double lean(double position, double price) const;
    // The markup factor of an order on `side`: 1 + factor u for a buy, 1 - factor u for a sell.
    Decimal multiplier(const std::string& side, const Decimal& position, const Decimal& price) const;
    // How far both quotes move down, a fraction of the price: shift_bp x u (the class short: up).
    Decimal shift(const Decimal& position, const Decimal& price) const;
    // For the placement line: the class's exposure and what it does to this side's markup.
    std::optional<std::string> note(const std::string& side, const Decimal& position, const Decimal& price) const;
    // "portfolio crypto: ETH-USD -300 $ (x1.3, 1 s ago), ours +750 $ -> +360 $, u +0.24: buys x1.07, sells x0.93".
    std::string status(const Decimal& position, const std::optional<Decimal>& price) const;

    // The other market's holding among `others`, or null.
    const Holding* other(const std::string& market) const;

    std::string dir;
    std::string market;
    std::string asset_class;
    double weight;
    double limit_usd;
    double factor;
    double shift_bp;
    double stale_s;
    double interval_s;
    std::function<double()> position;
    std::function<std::optional<double>()> mark;
    Clock clock;
    int64_t pid;
    std::string path;  // our file: dir/<market>.<pid>.json
    std::vector<Holding> others;  // the class's other markets, fresh, in the order the directory listed them
    double others_usd = 0.0;
    // file name -> (mtime ns, what it held) as last read; null: not a bot's file (or being replaced)
    std::map<std::string, std::pair<int64_t, std::shared_ptr<const Holding>>> files;

private:
    void tell(const std::string& key, const std::string& text);

    std::string name_;  // our file's name
    std::set<std::string> told_;  // files said once to be left out
    std::optional<double> mark_;  // our last mark, for the exposure between marks
    std::optional<std::pair<double, double>> written_;  // (position, when) of our file as last written
};

}  // namespace nmm
