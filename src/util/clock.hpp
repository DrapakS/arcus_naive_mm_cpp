// Clocks: the monotonic clock (seconds, as Python's time.monotonic) for
// intervals and pauses, and the wall clock for the exchanges' timestamps.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace nmm {

// A clock as the guards and pricers take it: seconds on some fixed scale.
using Clock = std::function<double()>;

inline double monotonic() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Wall clock, ns since the Unix epoch (time.time_ns).
inline int64_t time_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

// Wall clock, µs since the Unix epoch.
inline int64_t now_us() { return time_ns() / 1000; }

// Wall clock, seconds since the Unix epoch (time.time).
inline double wall_time() { return static_cast<double>(time_ns()) / 1e9; }

// CPU time of the process, seconds (time.process_time).
double process_time();

}  // namespace nmm
