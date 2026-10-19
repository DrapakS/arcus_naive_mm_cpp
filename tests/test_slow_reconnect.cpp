// Ported from tests/test_slow_reconnect.py: a connection to Arcus that answers slow is
// reconnected, one at a time, at most every 5 min.
#include <gtest/gtest.h>

#include "gateway_rig.hpp"

using namespace nmm;
using namespace nmm::test;

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {

void feed(std::deque<std::pair<double, double>>& samples, double now, double ms, int n = 20) {
    for (int i = 0; i < n; ++i) samples.emplace_back(now - i, ms);
}

}  // namespace

TEST(SlowReconnect, ASlowLaneIsReconnectedAndThenLeftAloneForAWhile) {
    GatewayLog log;
    Rig rig({.cancel_lanes = 1});
    ArcusGateway& gw = *rig.gw;
    gw.opt.slow_reconnect_ms = 50.0;
    Conn& main = gw.main_conn;
    Conn& lane = *gw.lanes[0];
    double now = monotonic() + 2 * SLOW_WINDOW_S;  // both up long enough to be judged
    feed(main.replies, now, 9.0);
    feed(lane.replies, now, 9.0, 9);  // too few of them to judge by
    EXPECT_EQ(gw.check_slow(now), nullptr);
    feed(lane.replies, now, 105.0, 11);  // 20 now, median 105: slow
    auto old = lane.ws;
    EXPECT_EQ(gw.check_slow(now), &lane);
    EXPECT_NE(log.text().find("!!! lane1 connection slow: cancel replies median 105 ms over the last 60 s (n 20, over 50 ms)"), std::string::npos)
        << log.text();
    ASSERT_TRUE(rig.wait([&] { return lane.ws != nullptr && lane.ws != old; }));
    EXPECT_TRUE(gw.ready);
    EXPECT_NE(main.ws, nullptr);  // main untouched
    double later = monotonic() + 2 * SLOW_WINDOW_S;
    feed(lane.replies, later, 105.0);
    EXPECT_EQ(gw.check_slow(later), nullptr);  // reconnected for this not 5 min ago
    feed(lane.replies, later + SLOW_RECONNECT_GAP_S, 105.0);
    EXPECT_EQ(gw.check_slow(later + SLOW_RECONNECT_GAP_S), &lane);
}

TEST(SlowReconnect, MainIsJudgedByOurOrderEventsTooAndNeverWhileAnotherIsDown) {
    Rig rig({.cancel_lanes = 1});
    ArcusGateway& gw = *rig.gw;
    gw.opt.slow_reconnect_ms = 50.0;
    Conn& main = gw.main_conn;
    Conn& lane = *gw.lanes[0];
    double now = monotonic() + 2 * SLOW_WINDOW_S;
    feed(main.replies, now, 8.0);  // cancels answered fast, but the orders channel 85 ms behind
    feed(main.events, now, 85.0);
    auto slow = main.slow(now, 50.0);
    ASSERT_TRUE(slow);
    EXPECT_EQ(*slow, (std::tuple<std::string, double, size_t> {"order events", 85.0, 20}));
    EXPECT_FALSE(main.slow(now + SLOW_WINDOW_S + 30, 50.0));  // all older than the window
    EXPECT_FALSE(main.slow(now, 100.0));
    auto ws = lane.ws;
    lane.ws = nullptr;  // the lane reconnecting: main waits
    EXPECT_EQ(gw.check_slow(now), nullptr);
    lane.ws = ws;
    EXPECT_EQ(gw.check_slow(monotonic() + 5), nullptr);  // main just connected: not judged yet
    EXPECT_EQ(gw.check_slow(now), &main);
    EXPECT_TRUE(rig.wait([&] { return gw.ready && main.ws != nullptr && main.events.empty(); }));
}
