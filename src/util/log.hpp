// Logging in the shape of Python's logging module, as the bot used it: named
// loggers in a hierarchy ("naive_mm.orders" under "naive_mm" under the root),
// levels, handlers, propagation; and every handler written by a thread of its
// own, so that a terminal paused with Ctrl+S, a stalled pipe or a slow disk
// never holds up the event loop. A handler whose thread falls more than
// QUEUE_SIZE lines behind drops new lines (and counts them).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nmm::logging {

enum Level : int { NOTSET = 0, DEBUG = 10, INFO = 20, WARNING = 30, ERROR = 40, CRITICAL = 50 };

constexpr size_t QUEUE_SIZE = 100'000;  // lines held for a writer that has fallen behind, then dropped

const char* level_name(int level);
// "DEBUG", "info", ... -> the level; throws std::invalid_argument for anything else.
int parse_level(std::string_view name);

struct Record {
    int level = INFO;
    std::string name;  // the logger's
    std::string message;
    int64_t created_us = 0;  // wall clock
};

// "%(asctime)s.%(msecs)03d %(levelname)-7s %(name)s: %(message)s", asctime "%H:%M:%S" local.
std::string format_record(const Record& record);

// A queue drained by a thread of its own; push never blocks (a full queue drops).
template <class T>
class BackgroundQueue {
public:
    explicit BackgroundQueue(std::function<void(T&)> consume, size_t maxsize = QUEUE_SIZE)
        : consume_(std::move(consume)), maxsize_(maxsize) {
        thread_ = std::thread([this] { run(); });
    }
    BackgroundQueue(const BackgroundQueue&) = delete;
    BackgroundQueue& operator=(const BackgroundQueue&) = delete;
    ~BackgroundQueue() { stop(); }

    bool push(T item) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || items_.size() >= maxsize_) {
                ++dropped_;
                return false;
            }
            items_.push_back(std::move(item));
            ++pending_;
        }
        cv_.notify_one();
        return true;
    }

    size_t dropped() const { return dropped_.load(); }

    // Wait (up to `timeout_s`) until everything pushed so far is written.
    void flush(double timeout_s = 2.0) {
        std::unique_lock lock(mutex_);
        idle_.wait_for(lock, std::chrono::duration<double>(timeout_s), [this] { return pending_ == 0; });
    }

    // Write out what is held (up to `timeout_s`) and stop the thread; a thread
    // stuck on a dead disk is left behind rather than waited for.
    void stop(double timeout_s = 2.0) {
        {
            std::unique_lock lock(mutex_);
            if (!thread_.joinable()) return;
            idle_.wait_for(lock, std::chrono::duration<double>(timeout_s), [this] { return pending_ == 0; });
            stopping_ = true;
        }
        cv_.notify_all();
        bool done;
        {
            std::unique_lock lock(mutex_);
            done = idle_.wait_for(lock, std::chrono::duration<double>(timeout_s), [this] { return exited_; });
        }
        if (done) thread_.join();
        else thread_.detach();
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            cv_.wait(lock, [this] { return stopping_ || !items_.empty(); });
            if (items_.empty() && stopping_) break;
            T item = std::move(items_.front());
            items_.pop_front();
            lock.unlock();
            try {
                consume_(item);
            } catch (...) {
            }
            lock.lock();
            if (--pending_ == 0) idle_.notify_all();
        }
        exited_ = true;
        idle_.notify_all();
    }

    std::function<void(T&)> consume_;
    size_t maxsize_;
    std::mutex mutex_;
    std::condition_variable cv_, idle_;
    std::deque<T> items_;
    size_t pending_ = 0;
    bool stopping_ = false, exited_ = false;
    std::atomic<size_t> dropped_ {0};
    std::thread thread_;
};

class Handler {
public:
    virtual ~Handler() = default;
    virtual void handle(const Record& record) = 0;
    virtual void flush(double timeout_s = 2.0) {}
    virtual void stop() {}
    int level = NOTSET;
};

// A handler that hands each record to its own thread, which formats and writes it.
class ThreadedHandler : public Handler {
public:
    // `write` gets each formatted line (with its newline); `format` defaults to format_record.
    ThreadedHandler(std::function<void(const std::string&)> write,
                    std::function<std::string(const Record&)> format = format_record);
    ~ThreadedHandler() override;
    void handle(const Record& record) override;
    void flush(double timeout_s = 2.0) override;
    void stop() override;
    size_t dropped() const;

private:
    std::function<void(const std::string&)> write_;
    std::function<std::string(const Record&)> format_;
    std::unique_ptr<BackgroundQueue<Record>> queue_;
};

// Records into a vector (tests).
class CaptureHandler : public Handler {
public:
    void handle(const Record& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
    std::vector<std::string> messages() {
        std::lock_guard lock(mutex);
        std::vector<std::string> out;
        for (auto& r : records) out.push_back(r.message);
        return out;
    }
    std::string text() {
        std::string out;
        for (auto& m : messages()) out += m + "\n";
        return out;
    }
    void clear() {
        std::lock_guard lock(mutex);
        records.clear();
    }
    std::mutex mutex;
    std::vector<Record> records;
};

class Logger {
public:
    Logger(std::string name, Logger* parent) : name_(std::move(name)), parent_(parent) {}

    const std::string& name() const { return name_; }
    void set_level(int level) { level_ = level; }
    int level() const { return level_; }
    int effective_level() const;
    bool enabled_for(int level) const { return level >= effective_level(); }
    void add_handler(std::shared_ptr<Handler> handler);
    void remove_handler(const std::shared_ptr<Handler>& handler);
    void clear_handlers();
    bool has_handlers() const;
    void set_propagate(bool propagate) { propagate_ = propagate; }

    void log(int level, std::string message);

    template <class... A>
    void log(int level, std::format_string<A...> f, A&&... args) {
        if (enabled_for(level)) log(level, std::format(f, std::forward<A>(args)...));
    }
    template <class... A>
    void debug(std::format_string<A...> f, A&&... args) { log(DEBUG, f, std::forward<A>(args)...); }
    template <class... A>
    void info(std::format_string<A...> f, A&&... args) { log(INFO, f, std::forward<A>(args)...); }
    template <class... A>
    void warning(std::format_string<A...> f, A&&... args) { log(WARNING, f, std::forward<A>(args)...); }
    template <class... A>
    void error(std::format_string<A...> f, A&&... args) { log(ERROR, f, std::forward<A>(args)...); }

private:
    std::string name_;
    Logger* parent_;
    int level_ = NOTSET;
    bool propagate_ = true;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Handler>> handlers_;
};

// The logger of that name ("" = the root), made on first use; it lives for the whole run.
Logger& get_logger(std::string_view name);
Logger& root();

}  // namespace nmm::logging

namespace nmm {
using Logger = logging::Logger;
inline Logger& get_logger(std::string_view name) { return logging::get_logger(name); }

// The loggers the bot writes to by name.
inline constexpr std::string_view ORDERS_LOGGER = "naive_mm.orders";  // every order event, as it happens
inline constexpr std::string_view QUOTES_LOGGER = "naive_mm.quotes";
inline constexpr std::string_view CONFIG_LOGGER = "naive_mm.config";

// Where quote lines and ladders go: a writer thread once start_stdout ran (the
// command line), plain stdout otherwise (tests).
void stdout_write(std::string text);
void start_stdout();
void stop_stdout();
// Quote lines go here instead of stdout when set (tests); nullptr = stdout.
void set_stdout_capture(std::function<void(const std::string&)> capture);

}  // namespace nmm
