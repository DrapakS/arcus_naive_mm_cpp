// The event loop, in the image of asyncio: one thread runs everything (a
// boost::asio::io_context), coroutines are tasks that can be cancelled,
// call_later / call_soon schedule callbacks, an Event wakes a waiting task.
#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <memory>
#include <string>

namespace nmm {

namespace asio = boost::asio;
template <class T = void>
using awaitable = asio::awaitable<T>;

// The loop the bot runs on (asyncio.get_running_loop): set once by whoever runs it.
asio::io_context& loop();
void set_loop(asio::io_context& ioc);

// A callback scheduled for later; cancel() keeps it from running.
class TimerHandle {
public:
    TimerHandle() = default;
    void cancel();
    explicit operator bool() const { return state_ != nullptr; }
    struct State;

private:
    friend TimerHandle call_later(double, std::function<void()>);
    std::shared_ptr<State> state_;
};

// Run `fn` after `delay_s` seconds (on the loop).
TimerHandle call_later(double delay_s, std::function<void()> fn);
// Run `fn` on the next turn of the loop.
void call_soon(std::function<void()> fn);
// Sleep `seconds` (cancellable).
awaitable<void> sleep(double seconds);

// A running coroutine; cancel() makes the operation it awaits fail (as CancelledError would).
class Task {
public:
    Task() = default;
    void cancel();
    bool done() const;
    explicit operator bool() const { return state_ != nullptr; }
    struct State;

    // Start `aw` on the loop now; a failure other than cancellation is logged
    // ("task <name> failed: ...") to the logger `log_name`.
    static Task spawn(awaitable<void> aw, std::string name = "", std::string log_name = "naive_mm");

private:
    std::shared_ptr<State> state_;
};

// asyncio.Event: set() wakes every task waiting in wait().
class Event {
public:
    void set();
    void clear() { flag_ = false; }
    bool is_set() const { return flag_; }
    awaitable<void> wait();

private:
    bool flag_ = false;
    std::shared_ptr<asio::steady_timer> timer_;
};

// Whether an exception is the cancellation of a task (operation aborted).
bool is_cancellation(const std::exception_ptr& e);

// Throw if the current task has been cancelled.
awaitable<void> check_cancelled();
// Whether the current task has been cancelled.
awaitable<bool> is_cancelled();

// The message of an exception ("" for none).
std::string what(const std::exception_ptr& e);

}  // namespace nmm
