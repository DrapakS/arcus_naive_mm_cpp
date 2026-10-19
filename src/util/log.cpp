#include "util/log.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <stdexcept>

#include "util/clock.hpp"
#include "util/pyfmt.hpp"

namespace nmm::logging {

const char* level_name(int level) {
    switch (level) {
    case DEBUG: return "DEBUG";
    case INFO: return "INFO";
    case WARNING: return "WARNING";
    case ERROR: return "ERROR";
    case CRITICAL: return "CRITICAL";
    default: return "NOTSET";
    }
}

int parse_level(std::string_view name) {
    std::string n = upper(name);
    if (n == "DEBUG") return DEBUG;
    if (n == "INFO") return INFO;
    if (n == "WARNING" || n == "WARN") return WARNING;
    if (n == "ERROR") return ERROR;
    if (n == "CRITICAL" || n == "FATAL") return CRITICAL;
    throw std::invalid_argument("Unknown level: '" + std::string(name) + "'");
}

std::string format_record(const Record& record) {
    std::time_t secs = static_cast<std::time_t>(record.created_us / 1'000'000);
    std::tm tm {};
    localtime_r(&secs, &tm);
    return std::format("{:02d}:{:02d}:{:02d}.{:03d} {:<7} {}: {}\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                       (record.created_us % 1'000'000) / 1000, level_name(record.level), record.name, record.message);
}

ThreadedHandler::ThreadedHandler(std::function<void(const std::string&)> write,
                                 std::function<std::string(const Record&)> format)
    : write_(std::move(write)), format_(std::move(format)) {
    queue_ = std::make_unique<BackgroundQueue<Record>>([this](Record& r) { write_(format_(r)); });
}

ThreadedHandler::~ThreadedHandler() { stop(); }

void ThreadedHandler::handle(const Record& record) { queue_->push(record); }

void ThreadedHandler::flush(double timeout_s) { queue_->flush(timeout_s); }

void ThreadedHandler::stop() { queue_->stop(); }

size_t ThreadedHandler::dropped() const { return queue_->dropped(); }

int Logger::effective_level() const {
    for (const Logger* l = this; l != nullptr; l = l->parent_)
        if (l->level_ != NOTSET) return l->level_;
    return WARNING;
}

void Logger::add_handler(std::shared_ptr<Handler> handler) {
    std::lock_guard lock(mutex_);
    handlers_.push_back(std::move(handler));
}

void Logger::remove_handler(const std::shared_ptr<Handler>& handler) {
    std::lock_guard lock(mutex_);
    handlers_.erase(std::remove(handlers_.begin(), handlers_.end(), handler), handlers_.end());
}

void Logger::clear_handlers() {
    std::lock_guard lock(mutex_);
    handlers_.clear();
}

bool Logger::has_handlers() const {
    std::lock_guard lock(mutex_);
    return !handlers_.empty();
}

void Logger::log(int level, std::string message) {
    if (!enabled_for(level)) return;
    Record record {level, name_, std::move(message), now_us()};
    for (Logger* l = this; l != nullptr; l = l->propagate_ ? l->parent_ : nullptr) {
        std::vector<std::shared_ptr<Handler>> handlers;
        {
            std::lock_guard lock(l->mutex_);
            handlers = l->handlers_;
        }
        for (auto& h : handlers)
            if (level >= h->level) h->handle(record);
    }
}

namespace {
std::recursive_mutex registry_mutex;
std::map<std::string, std::unique_ptr<Logger>, std::less<>>& registry() {
    static std::map<std::string, std::unique_ptr<Logger>, std::less<>> loggers;
    return loggers;
}
}  // namespace

Logger& root() { return get_logger(""); }

Logger& get_logger(std::string_view name) {
    std::lock_guard lock(registry_mutex);
    auto& loggers = registry();
    if (auto it = loggers.find(name); it != loggers.end()) return *it->second;
    // the parent: the longest dotted prefix, made as needed; the root for a top-level name
    Logger* parent = nullptr;
    if (!name.empty()) {
        auto dot = name.rfind('.');
        parent = &get_logger(dot == std::string_view::npos ? std::string_view() : name.substr(0, dot));
    }
    auto logger = std::make_unique<Logger>(std::string(name), parent);
    Logger& ref = *logger;
    loggers.emplace(std::string(name), std::move(logger));
    return ref;
}

}  // namespace nmm::logging

namespace nmm {

namespace {
std::unique_ptr<logging::BackgroundQueue<std::string>> stdout_queue;
std::function<void(const std::string&)> stdout_capture;
}  // namespace

void stdout_write(std::string text) {
    if (stdout_capture) {
        stdout_capture(text);
        return;
    }
    if (stdout_queue) {
        stdout_queue->push(std::move(text));
        return;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

void start_stdout() {
    stdout_queue = std::make_unique<logging::BackgroundQueue<std::string>>([](std::string& text) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    });
}

void stop_stdout() {
    if (stdout_queue) {
        stdout_queue->stop();
        stdout_queue.reset();
    }
}

void set_stdout_capture(std::function<void(const std::string&)> capture) { stdout_capture = std::move(capture); }

}  // namespace nmm
