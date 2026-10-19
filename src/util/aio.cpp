#include "util/aio.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>

#include "util/log.hpp"

namespace nmm {

namespace {
asio::io_context* current_loop = nullptr;

asio::steady_timer::duration to_duration(double seconds) {
    if (seconds < 0) seconds = 0;
    return std::chrono::duration_cast<asio::steady_timer::duration>(std::chrono::duration<double>(seconds));
}
}  // namespace

asio::io_context& loop() {
    if (current_loop == nullptr) throw std::logic_error("no event loop is running");
    return *current_loop;
}

void set_loop(asio::io_context& ioc) { current_loop = &ioc; }

struct TimerHandle::State {
    explicit State(asio::io_context& ioc) : timer(ioc) {}
    asio::steady_timer timer;
    bool cancelled = false;
};

void TimerHandle::cancel() {
    if (state_ != nullptr) {
        state_->cancelled = true;
        state_->timer.cancel();
        state_.reset();
    }
}

TimerHandle call_later(double delay_s, std::function<void()> fn) {
    TimerHandle handle;
    handle.state_ = std::make_shared<TimerHandle::State>(loop());
    handle.state_->timer.expires_after(to_duration(delay_s));
    handle.state_->timer.async_wait([state = handle.state_, fn = std::move(fn)](const boost::system::error_code& ec) {
        if (ec || state->cancelled) return;
        state->cancelled = true;  // ran: a later cancel() is a no-op
        fn();
    });
    return handle;
}

void call_soon(std::function<void()> fn) { asio::post(loop(), std::move(fn)); }

awaitable<void> sleep(double seconds) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(to_duration(seconds));
    co_await timer.async_wait(asio::use_awaitable);
}

struct Task::State {
    asio::cancellation_signal signal;
    bool done = false;
    std::string name;
};

void Task::cancel() {
    if (state_ != nullptr && !state_->done) state_->signal.emit(asio::cancellation_type::terminal);
}

bool Task::done() const { return state_ == nullptr || state_->done; }

bool is_cancellation(const std::exception_ptr& e) {
    if (!e) return false;
    try {
        std::rethrow_exception(e);
    } catch (const boost::system::system_error& err) {
        return err.code() == asio::error::operation_aborted;
    } catch (...) {
        return false;
    }
}

Task Task::spawn(awaitable<void> aw, std::string name, std::string log_name) {
    Task task;
    task.state_ = std::make_shared<State>();
    task.state_->name = std::move(name);
    auto state = task.state_;
    asio::co_spawn(loop(), std::move(aw),
                   asio::bind_cancellation_slot(state->signal.slot(), [state, log_name](std::exception_ptr e) {
                       state->done = true;
                       if (!e || is_cancellation(e)) return;
                       try {
                           std::rethrow_exception(e);
                       } catch (const std::exception& err) {
                           get_logger(log_name).error("task {} failed: {}", state->name, err.what());
                       } catch (...) {
                           get_logger(log_name).error("task {} failed", state->name);
                       }
                   }));
    return task;
}

void Event::set() {
    flag_ = true;
    if (timer_) timer_->cancel();
}

awaitable<void> Event::wait() {
    while (!flag_) {
        if (!timer_) timer_ = std::make_shared<asio::steady_timer>(co_await asio::this_coro::executor);
        auto timer = timer_;
        timer->expires_at(asio::steady_timer::time_point::max());
        co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
        co_await check_cancelled();
    }
}

awaitable<void> check_cancelled() {
    auto state = co_await asio::this_coro::cancellation_state;
    if (state.cancelled() != asio::cancellation_type::none)
        throw boost::system::system_error(asio::error::operation_aborted);
}

}  // namespace nmm

namespace nmm {

awaitable<bool> is_cancelled() {
    auto state = co_await asio::this_coro::cancellation_state;
    co_return state.cancelled() != asio::cancellation_type::none;
}

std::string what(const std::exception_ptr& e) {
    if (!e) return "";
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& err) {
        return err.what();
    } catch (...) {
        return "unknown error";
    }
}

}  // namespace nmm
