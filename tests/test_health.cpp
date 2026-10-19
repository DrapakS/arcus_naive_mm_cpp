#include <gtest/gtest.h>

#include <thread>

#include "feed_helpers.hpp"
#include "health.hpp"
#include "test_util.hpp"

using namespace nmm;
using namespace nmm::test;

TEST(Health, DelaysReportPercentilesOfABoundedSampleAndTheExactMax) {
    Delays d(100);
    EXPECT_FALSE(d.take());  // nothing seen
    for (int i = 0; i < 10'000; ++i) d.add(static_cast<double>(i % 100));
    d.add(5'000.0);
    EXPECT_EQ(d.count, 10'001);
    EXPECT_EQ(d.sample.size(), 100u);
    EXPECT_EQ(d.last, 5'000.0);
    std::string text = *d.take();
    double p50 = std::stod(text.substr(4));
    EXPECT_TRUE(30 <= p50 && p50 <= 70) << text;  // a uniform sample: the median is near 50
    EXPECT_TRUE(text.ends_with("/ max 5000 ms (n 10001)")) << text;
    EXPECT_FALSE(d.take());  // each take starts over
    EXPECT_EQ(d.count, 0);
    d.add(1.5);
    EXPECT_EQ(*d.take(), "p50 1.5 / p99 1.5 / max 1.5 ms (n 1)");
}

TEST(Health, ALongLagIsLoggedAtMostEverySoOftenWithTheOnesBetween) {
    LogCapture logs("test.health");
    LoopWatch watch(get_logger("test.health"), LOOP_TICK_S, 100, 60);
    watch.observe(5.0);
    watch.observe(250.0);
    watch.observe(180.0);  // within the minute: counted, not logged
    EXPECT_EQ(watch.stalls, 2);
    EXPECT_EQ(logs.messages(), (std::vector<std::string> {"!!! event loop 250 ms late: feeds, guards and cancels all waited that long"}));
    watch.stall_logged -= 61;  // a minute later
    watch.observe(120.0);
    EXPECT_EQ(logs.messages().back(),
              "!!! event loop 120 ms late: feeds, guards and cancels all waited that long "
              "(1 more of 100 ms or longer since the last such line)");
    EXPECT_TRUE(watch.lags.take()->ends_with("max 250 ms (n 4)"));
}

TEST(Health, TheWatchSeesABlockedLoop) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    LoopWatch watch(get_logger("test.health"), 0.005);
    Task task = Task::spawn(watch.run());
    run_for(ioc, 0.02);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));  // the loop blocked, as by a long call on it
    run_for(ioc, 0.02);
    task.cancel();
    run_for(ioc, 0.01);
    EXPECT_GE(watch.stalls, 1);
    EXPECT_GE(watch.lags.max, 100);
    EXPECT_TRUE(watch.cpu());
}
