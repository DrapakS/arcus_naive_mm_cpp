// Helpers of the order book and feed tests.
#pragma once

#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <vector>

#include "net/net.hpp"
#include "orderbook.hpp"
#include "util/json.hpp"
#include "util/log.hpp"

namespace nmm::test {

// JSON written in the test.
inline Json J(const char* text) { return parse_json(text); }

using Levels = std::vector<Level>;

// A connection that is there and does nothing: a link counted as up.
class DummyWs : public WsConnection {
public:
    awaitable<std::optional<std::string>> read() override { co_return std::nullopt; }
    bool send(std::string, std::function<void(const std::string&)> done) override {
        if (done) done("");
        return true;
    }
    void close() override {}
    awaitable<void> close_gracefully(double) override { co_return; }
    bool closed() const override { return false; }
    int close_code() const override { return 0; }
};

// Let the loop run what is ready, a few turns over (asyncio.sleep(0) five times).
inline void settle(boost::asio::io_context& ioc) {
    for (int i = 0; i < 5; ++i) {
        ioc.restart();
        ioc.poll();
    }
}

// The records of one logger (at `level` and up) while it lives.
class LogCapture {
public:
    explicit LogCapture(std::string_view name, int level = logging::INFO)
        : logger_(get_logger(name)), previous_(logger_.level()), handler_(std::make_shared<logging::CaptureHandler>()) {
        logger_.set_level(level);
        logger_.add_handler(handler_);
    }
    ~LogCapture() {
        logger_.remove_handler(handler_);
        logger_.set_level(previous_);
    }
    std::vector<std::string> messages() { return handler_->messages(); }
    std::vector<std::string> messages_at(int level) {
        std::lock_guard lock(handler_->mutex);
        std::vector<std::string> out;
        for (auto& r : handler_->records)
            if (r.level == level) out.push_back(r.message);
        return out;
    }

private:
    Logger& logger_;
    int previous_;
    std::shared_ptr<logging::CaptureHandler> handler_;
};

}  // namespace nmm::test
