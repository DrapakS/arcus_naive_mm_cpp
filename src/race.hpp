// Several connections carrying the same stream: which one delivers each item first, and how far
// behind the first copy the others come.
//
// The server can hand out a connection that delivers everything late and stays so (paper pair
// Python/C++, 19 Oct: after a reconnect, one connection to the same Cloudflare address had every
// Arcus trade 73-78 ms after the other's, for the hour that followed, in the packets on the network
// card; the run before, the C++ bot's connections did the same). A connection is not slow by itself,
// only behind another one that carries the same stream, so each copy is timed against the first copy
// of the same item: the first copy counts 0 ms, a later one how long after it it came. A link whose
// median over the last SLOW_LINK_WINDOW_S is past the threshold while another link is up is
// reconnected, and a new connection usually comes on another route.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/clock.hpp"

namespace nmm {

inline constexpr double SLOW_LINK_WINDOW_S = 120.0;  // the median is over the copies of this long (the Arcus trades of ETH
inline constexpr size_t SLOW_LINK_MIN_SAMPLES = 15;  // or GLD: some 20-40 frames); fewer copies than this in the window: no judgement
inline constexpr double SLOW_LINK_SETTLE_S = 30.0;  // a link connected this recently is not judged
inline constexpr double SLOW_LINK_GAP_S = 120.0;  // a link is reconnected for being slow at most this often
inline constexpr double SLOW_LINK_CHECK_S = 5.0;
// At most one copy a link this often goes into its median: a minute of a fast stream (futures, 1000-3000
// tickers a second in a burst) still fits the window, so a burst cannot judge a link on a second. (Tests set it.)
inline double SAMPLE_EVERY_S = 0.02;
inline constexpr size_t COPY_KEYS = 4096;  // items remembered for timing the later copies

// When the first copy of each recent item arrived, by the item's key.
class FirstCopies {
public:
    explicit FirstCopies(size_t keep = COPY_KEYS) : keep_(keep) {}
    // (the first copy of `key`?, ms after the first copy: 0 for the first itself)
    std::pair<bool, double> arrived(int64_t key, int64_t recv_us);
    void clear();

private:
    size_t keep_;
    std::unordered_map<int64_t, int64_t> first_;
    std::deque<int64_t> order_;
};

// One connection's copies: (monotonic s, ms behind the first copy).
class LinkLag {
public:
    LinkLag();
    void connected();
    void add(double ms);
    // (median ms, copies) over the window; nullopt without enough copies.
    std::optional<std::pair<double, size_t>> median(std::optional<double> now = {}) const;
    // (median, copies) when this link is slow and may be reconnected now, else nullopt.
    std::optional<std::pair<double, size_t>> judge(double threshold_ms, std::optional<double> now = {}) const;
    void dropped_for_slowness(std::optional<double> now = {});

    std::deque<std::pair<double, double>> samples;  // at most 6000: 2 min at one each 20 ms
    double connected_at;
    double slow_at = -SLOW_LINK_GAP_S;  // when it was last reconnected for being slow
    int64_t slow_drops = 0;

private:
    double sampled_at_ = -1e300;
};

// (link, median ms, copies) of the first link that is slow while another one is up, or nullopt.
// A link has up(), `lag` (a LinkLag) and `slow_drop` (being closed already).
template <class L>
std::optional<std::tuple<L*, double, size_t>> slow_link(std::vector<L>& links, double threshold_ms, std::optional<double> now = {}) {
    for (auto& link : links) {
        if (!link.up() || link.slow_drop) continue;
        bool other_up = false;
        for (const auto& other : links)
            if (&other != &link && other.up()) other_up = true;
        if (!other_up) continue;
        if (auto slow = link.lag.judge(threshold_ms, now)) return std::make_tuple(&link, slow->first, slow->second);
    }
    return std::nullopt;
}

// "behind 0/74ms": each link's median over the window ("-" without enough copies).
std::string lag_summary(const std::vector<const LinkLag*>& lags);

}  // namespace nmm
