// How far behind real time the bot runs, for the logs.
//
// Three measures. The event loop's lag: how late a short timer fires; a loop
// busy elsewhere (CPU shared with other bots, a long callback, a blocking call)
// delays every feed and every cancel at once. Each feed's delay: from the
// exchange's own timestamp on a message to the moment we read it, network plus
// whatever the loop was behind. And the process's share of a CPU core.
//
// Overnight 14-15 Oct the BTC bot read the exchange's messages 98 ms late at the
// 75th percentile (14 ms is the floor) while its cancels reached the exchange in
// 15 ms, and its guards saw Binance moves 50-80 ms after the trades printed: the
// race with the takers that hit our quotes was lost on the way in. These lines
// show where: a lagging loop points at the host, one late feed at its route.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "util/aio.hpp"
#include "util/log.hpp"

namespace nmm {

constexpr size_t RESERVOIR = 1024;  // delays kept per report for the percentiles: a uniform sample of them all
constexpr double LOOP_TICK_S = 0.01;  // the loop-lag probe's timer
constexpr double STALL_MS = 100.0;  // a lag this long is logged on its own line
constexpr double STALL_LOG_EVERY_S = 10.0;  // at most one such line this often; the stalls in between are counted

// Delays (ms) seen since the last take: how many, a uniform sample of them for
// the percentiles (reservoir sampling: memory and time per value stay flat
// however many arrive) and the exact maximum. `last` is the newest.
class Delays {
public:
    explicit Delays(size_t size = RESERVOIR, std::function<double()> rng = {});
    void add(double ms);
    // "p50 2 / p99 15 / max 120 ms (n 12000)" since the last take, none without
    // any; and start over.
    std::optional<std::string> take();

    size_t size;
    std::function<double()> rng;  // uniform in [0, 1)
    int64_t count = 0;
    std::vector<double> sample;
    double max;
    std::optional<double> last;
};

// Times a LOOP_TICK_S timer, forever: how much later than asked it fires is the
// loop's lag. Lags of `stall_ms` or more get a line of their own (at most every
// `stall_every_s`, with the count of the ones between), and cpu() gives the
// process's CPU share since it was last asked. Every lag also goes to `on_lag`
// (ms), when given: the lag guard's reading.
class LoopWatch {
public:
    LoopWatch(Logger& log, double tick_s = LOOP_TICK_S, double stall_ms = STALL_MS,
              double stall_every_s = STALL_LOG_EVERY_S, std::function<void(double)> on_lag = {});
    awaitable<void> run();
    void observe(double lag_ms);
    // The process's CPU time over the wall time since the last call (1.0 = one core).
    std::optional<double> cpu();

    Logger& log;
    std::function<void(double)> on_lag;
    double tick_s;
    double stall_ms;
    double stall_every_s;
    Delays lags;
    int64_t stalls = 0;  // lags of stall_ms or more, all told
    int64_t unlogged = 0;  // of them, the ones since the last stall line
    double stall_logged;  // monotonic time of the last stall line

private:
    double cpu_time_;
    double cpu_wall_;
};

}  // namespace nmm
