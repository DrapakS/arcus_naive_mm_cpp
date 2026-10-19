// Ported from tests/test_late_open.py. ETH live, 19 Oct 14:04 and 14:13: an order filled 69 ms after
// placing (public trades), its OPEN came 12 ms after that and was taken for a stray, and the userFills
// of the same trade counted again: the tracked position 0.312 ETH off the exchange's, and the bot went
// on quoting by it.
#include <gtest/gtest.h>

#include "gateway_rig.hpp"

using namespace nmm;
using namespace nmm::test;
using D = Decimal;
using Access = ArcusGatewayTestAccess;

TEST(LateOpen, ALateOpenOfAnOrderSeenFilledWholeIsNoStrayAndItsUserFillCountsOnce) {
    GatewayLog log;
    Rig rig;
    rig.quote();
    OrderPtr buy = rig.gw->order();
    ASSERT_TRUE(buy);
    JsonObject stale = rig.fake->order_by_client_id(buy->client_id);  // as it stood: OPEN, nothing filled
    rig.fake->fill(buy->client_id, "0.001", false, true, false, "t1");  // the public trade first
    ASSERT_TRUE(rig.wait([&] { return buy->state == OrderState::FILLED && rig.gw->position() == D("0.001"); }));
    rig.fake->push(stale);  // its OPEN, late
    rig.fake->fill(buy->client_id, "0.001", false, false, true, "t1");
    ASSERT_TRUE(rig.wait([&] {
        for (const auto& line : log.lines())
            if (line.find("userFills BUY 0.001") != std::string::npos) return true;
        return false;
    })) << log.text();
    EXPECT_EQ(rig.gw->position(), D("0.001"));  // once
    for (const auto& line : log.lines()) {
        EXPECT_EQ(line.find("not placed by this run"), std::string::npos) << line;
        EXPECT_EQ(line.find("stray"), std::string::npos) << line;
    }
}

TEST(LateOpen, AMismatchWithTheExchangeThatHoldsTakesItsPosition) {
    Patch settle(POSITION_SETTLE_S, 0.05);
    Patch adopt(POSITION_ADOPT_S, 0.1);
    GatewayLog log;
    Rig rig;
    ArcusGateway& gw = *rig.gw;
    EXPECT_EQ(gw.position(), 0);
    Access::on_positions(gw, parse_json(R"({"isSnapshot": false, "marketId": 1, "side": "SHORT", "size": "-0.312"})"));
    ASSERT_TRUE(rig.wait([&] { return gw.position() == D("-0.312"); }));
    auto has = [&](const std::string& part) {
        for (const auto& line : log.lines())
            if (line.find(part) != std::string::npos) return true;
        return false;
    };
    EXPECT_TRUE(has("!!! exchange reports position -0.312, tracked 0")) << log.text();
    EXPECT_TRUE(has("!!! taking the exchange's position -0.312 (tracked 0 for")) << log.text();
    // an update that agrees: nothing more
    Access::on_positions(gw, parse_json(R"({"isSnapshot": false, "marketId": 1, "side": "SHORT", "size": "-0.312"})"));
}

TEST(LateOpen, AMismatchThatMovesBeforeItSettlesIsNotTaken) {
    Patch settle(POSITION_SETTLE_S, 0.05);
    Patch adopt(POSITION_ADOPT_S, 0.3);
    Rig rig;
    ArcusGateway& gw = *rig.gw;
    Access::on_positions(gw, parse_json(R"({"isSnapshot": false, "marketId": 1, "side": "LONG", "size": "0.001"})"));
    ASSERT_TRUE(rig.wait([&] { return Access::position_adopt(gw); }));
    Access::on_positions(gw, parse_json(R"({"isSnapshot": false, "marketId": 1, "side": "FLAT", "size": "0"})"));  // agrees now
    ASSERT_TRUE(rig.wait([&] { return !Access::position_check(gw); }));
    rig.sleep(0.4);
    EXPECT_EQ(gw.position(), 0);
}
