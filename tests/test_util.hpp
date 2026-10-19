// Helpers shared by the tests: a clock moved by hand, and running the event loop
// until a condition holds.
#pragma once

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <functional>
#include <memory>

#include "util/aio.hpp"
#include "util/clock.hpp"

namespace nmm::test {

// A clock tests move by hand: clock() reads it, advance moves it. Copies share the time.
struct FakeClock {
    std::shared_ptr<double> now = std::make_shared<double>(1000.0);
    explicit FakeClock(double start = 1000.0) { *now = start; }
    double operator()() const { return *now; }
    void advance(double seconds) const { *now += seconds; }
    void set(double t) const { *now = t; }
    // As a Clock (std::function) that follows this one.
    Clock fn() const {
        auto p = now;
        return [p] { return *p; };
    }
};

// Run `ioc` (made the loop) until `predicate` holds; false if `timeout_s` passed first.
inline bool run_until(boost::asio::io_context& ioc, const std::function<bool()>& predicate, double timeout_s = 5.0) {
    set_loop(ioc);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        ioc.restart();
        ioc.run_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Run `ioc` for `seconds` of real time.
inline void run_for(boost::asio::io_context& ioc, double seconds) {
    set_loop(ioc);
    ioc.restart();
    ioc.run_for(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds)));
}

}  // namespace nmm::test
