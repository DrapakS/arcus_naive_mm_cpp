// A live gateway connected to the fake exchange, plus the strategy (Rig of
// tests/test_trading_gateway.py), and what the gateway tests share with it.
#pragma once

#include <boost/asio/co_spawn.hpp>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "fake_arcus_trading.hpp"
#include "test_util.hpp"
#include "trading/credentials.hpp"
#include "trading/gateway.hpp"
#include "trading/market.hpp"
#include "trading/strategy.hpp"
#include "util/log.hpp"
#include "util/pyfmt.hpp"
#include "view.hpp"

namespace nmm::test::gw {

// RFC 8032 test 1 key pair and the EIP-55 example address: public test values.
inline const std::string SECRET = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
inline const std::string PUBLIC = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
inline const std::string ADDRESS = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";

inline MarketSpec btc_market() {
    return MarketSpec::from_json(parse_json(R"({
        "marketId": 1, "marketDisplayName": "BTC-USD", "tickSize": "0.1", "stepSize": "0.00000001",
        "tickTiers": [{"upToPrice": "500000", "tick": "0.1"}, {"upToPrice": "1000000", "tick": "0.2"}, {"tick": "5"}],
        "minOrderSize": "0.0001", "minOrderNotional": "5", "maxOrderSize": "10000"})"));
}

// A one-level Arcus book and a fair bid: what the strategy sees.
inline MarketView view(std::optional<double> fair, double bid = 99'990.0, double ask = 99'990.1) {
    MarketView v;
    v.venue = std::make_shared<BookTop>(BookTop {"arcus", "BTC-USD", {{bid, 1.0}}, {{ask, 1.0}}, 1, std::nullopt, 0});
    v.fair_bid = fair;
    return v;
}

// A key pair the fake exchange does not know.
inline std::string unregistered_key() {
    std::random_device rd;
    std::string hex;
    static const char* digits = "0123456789abcdef";
    for (int i = 0; i < 64; ++i) hex += digits[rd() & 15];
    return hex;
}

// The records of the gateway's two loggers, in the order they were written; the text as
// formatted when logged (an order's str() changes as it moves on).
class GatewayLog {
public:
    explicit GatewayLog(int level = logging::INFO) : handler_(std::make_shared<logging::CaptureHandler>()) {
        for (auto* l : loggers()) {
            levels_.push_back(l->level());
            l->set_level(level);
            l->add_handler(handler_);
        }
    }
    ~GatewayLog() {
        auto ls = loggers();
        for (size_t i = 0; i < ls.size(); ++i) {
            ls[i]->remove_handler(handler_);
            ls[i]->set_level(levels_[i]);
        }
    }
    std::vector<std::string> lines() { return handler_->messages(); }
    std::vector<logging::Record> records() {
        std::lock_guard lock(handler_->mutex);
        return handler_->records;
    }
    std::string text() { return handler_->text(); }
    void clear() { handler_->clear(); }

private:
    static std::vector<Logger*> loggers() { return {&get_logger("naive_mm.arcus-trading"), &get_logger(ORDERS_LOGGER)}; }
    std::shared_ptr<logging::CaptureHandler> handler_;
    std::vector<int> levels_;
};

// The lines that match `pattern` whole (re.fullmatch).
inline std::vector<std::smatch> matching(const std::vector<std::string>& lines, const std::string& pattern) {
    std::regex re(pattern);
    std::vector<std::smatch> out;
    for (const auto& line : lines) {
        std::smatch m;
        if (std::regex_match(line, m, re)) out.push_back(m);
    }
    return out;
}

inline bool contains(const std::vector<std::string>& lines, const std::string& line) {
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

// A module constant shortened for one test.
class Patch {
public:
    Patch(double& value, double patched) : value_(value), saved_(value) { value = patched; }
    ~Patch() { value_ = saved_; }

private:
    double& value_;
    double saved_;
};

struct RigOptions {
    std::optional<std::string> signing_key;  // another key than the one the exchange knows
    double dms_timeout = 5.0;
    bool wait_ready = true;
    int cancel_lanes = 0;
    double presign_cancel = 0.0;
    std::vector<OrderRow> open_orders, closed_orders;
};

class Rig {
public:
    explicit Rig(RigOptions o = {}) {
        set_loop(ioc);
        fake = std::make_unique<FakeArcusTrading>(ioc, ADDRESS, 1, PUBLIC, o.open_orders, o.closed_orders);
        fake->start();
        ArcusGateway::Options options;
        options.ws_url = fake->ws_url();
        options.dms_timeout = o.dms_timeout;
        options.cancel_lanes = o.cancel_lanes;
        options.presign_cancel = o.presign_cancel;
        options.rest_url = fake->rest_url();
        gw = std::make_unique<ArcusGateway>(btc_market(), ArcusCredentials::make(ADDRESS, 1, o.signing_key.value_or(SECRET)), options);
        StrategyOptions so;
        so.markup = 0.001;
        so.tol = 0.0002;
        so.order_size = 0.001;
        strategy = std::make_unique<QuoteStrategy>(gw.get(), btc_market(), so);
        task = Task::spawn(gw->run(), "trading");
        if (o.wait_ready) {
            bool ready = wait([this] {
                if (!gw->ready) return false;
                for (auto& lane : gw->lanes)
                    if (lane->ws == nullptr) return false;
                return true;
            });
            EXPECT_TRUE(ready) << "the gateway never got ready";
        }
    }

    ~Rig() { stop(); }

    // As the task's cancellation in Python: the orders cancelled on the way out, then the connections closed.
    void stop() {
        if (stopped) return;
        stopped = true;
        bool done = false;
        boost::asio::co_spawn(ioc, gw->shutdown(), [&](std::exception_ptr) { done = true; });
        run_until(ioc, [&] { return done; }, 10.0);
        task.cancel();
        run_for(ioc, 0.05);
        fake->close();
        run_for(ioc, 0.02);
    }

    bool wait(const std::function<bool()>& predicate, double timeout_s = 5.0) { return run_until(ioc, predicate, timeout_s); }
    void sleep(double seconds) { run_for(ioc, seconds); }

    // The strategy on a view, then until nothing is in flight.
    void quote(double fair = 100'000.0, double bid = 99'990.0, double ask = 99'990.1) {
        strategy->evaluate(view(fair, bid, ask));
        EXPECT_TRUE(wait([this] { return !gw->busy(); }));
    }

    int count_posts(const std::string& method) const {
        int n = 0;
        for (auto& [m, body] : fake->posts) n += m == method;
        return n;
    }

    boost::asio::io_context ioc;
    std::unique_ptr<FakeArcusTrading> fake;
    std::unique_ptr<ArcusGateway> gw;
    std::unique_ptr<QuoteStrategy> strategy;
    Task task;
    bool stopped = false;
};

}  // namespace nmm::test

namespace nmm {
// The gateway's private parts, for the tests.
struct ArcusGatewayTestAccess {
    static void on_positions(ArcusGateway& gw, const Json& contents) { gw.on_positions(contents); }
    static void on_order_update(ArcusGateway& gw, const Json& data) { gw.on_order_update(data); }
};
}  // namespace nmm

// Each test helper header keeps its names apart (several define a FakeGateway or a view()).
namespace nmm::test {
using namespace nmm::test::gw;
}  // namespace nmm::test
