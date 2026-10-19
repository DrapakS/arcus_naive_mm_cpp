// Markets with trading hours (Arcus RWA perps): open or closed (the session clock itself; the
// strategy and market spec sides of tests/test_session.py are elsewhere).
#include <gtest/gtest.h>

#include "test_util.hpp"
#include "trading/opens.hpp"
#include "trading/session.hpp"
#include "util/log.hpp"

using nmm::Hours;
using nmm::SessionClock;
using nmm::test::FakeClock;

namespace {

// NVDA-USD's hours as Arcus lists them: 04:00-20:00 New York
const nmm::Json RTH = nmm::parse_json(
    R"({"startSecondsOfDay": 14400, "endSecondsOfDay": 72000, "timezone": "America/New_York", "isOvernight": false})");

double at(int y, int mo, int d, int h, int mi, int s = 0) {
    using namespace std::chrono;
    auto day = local_days {year {y} / month {static_cast<unsigned>(mo)} / std::chrono::day {static_cast<unsigned>(d)}};
    return nmm::local_to_unix(locate_zone("America/New_York"),
                              day.time_since_epoch().count() * 86400LL + h * 3600 + mi * 60 + s);
}

struct Capture {
    std::shared_ptr<nmm::logging::CaptureHandler> handler = std::make_shared<nmm::logging::CaptureHandler>();
    nmm::Logger& logger = nmm::get_logger(nmm::ORDERS_LOGGER);
    int level = logger.level();
    Capture() {
        logger.set_level(nmm::logging::INFO);
        logger.add_handler(handler);
    }
    ~Capture() {
        logger.remove_handler(handler);
        logger.set_level(level);
    }
    std::vector<std::string> messages() { return handler->messages(); }
};

}  // namespace

TEST(Session, TheExtendedUsSessionIsOpen04To20NewYorkOnWeekdays) {
    SessionClock clock(*Hours::from_json(&RTH));
    EXPECT_EQ(clock.by_clock(at(2026, 10, 16, 3, 59, 59)), false);  // Friday before 04:00
    EXPECT_EQ(clock.by_clock(at(2026, 10, 16, 4, 0)), true);
    EXPECT_EQ(clock.by_clock(at(2026, 10, 16, 19, 59, 59)), true);
    EXPECT_EQ(clock.by_clock(at(2026, 10, 16, 20, 0)), false);
    EXPECT_EQ(clock.by_clock(at(2026, 10, 17, 12, 0)), false);  // Saturday
    EXPECT_EQ(clock.by_clock(at(2026, 10, 18, 12, 0)), false);  // Sunday
    EXPECT_EQ(clock.by_clock(at(2026, 10, 19, 4, 0)), true);  // Monday
    EXPECT_EQ(clock.by_clock(at(2026, 11, 2, 4, 30)), true);  // after the clocks went back: still New York time
    EXPECT_EQ(Hours::from_json(&RTH)->describe(), "04:00-20:00 America/New_York, Monday to Friday");
    EXPECT_FALSE(Hours::from_json(nullptr));
    nmm::Json no_hours = nmm::parse_json(R"({"timezone": "UTC"})");
    EXPECT_FALSE(Hours::from_json(&no_hours));
    nmm::Json no_zone = nmm::parse_json(R"({"startSecondsOfDay": 0, "endSecondsOfDay": 3600, "timezone": null})");
    EXPECT_EQ(Hours::from_json(&no_zone)->tz, "UTC");
}

TEST(Session, ClosedWhenEitherTheClockOrArcusSaysSo) {
    Capture capture;
    FakeClock now(at(2026, 10, 16, 12, 0));  // Friday noon
    SessionClock clock(*Hours::from_json(&RTH), false, now.fn(), ", markup x2");
    EXPECT_FALSE(clock.outside());
    EXPECT_TRUE(capture.messages().empty());  // open at the start: nothing to say
    clock.report(true);  // a holiday: Arcus knows, the clock does not
    EXPECT_TRUE(clock.outside());
    EXPECT_EQ(capture.messages().back(),
              "--- trading session (04:00-20:00 America/New_York, Monday to Friday): closed, markup x2");
    clock.report(std::nullopt);  // a failed look keeps the last answer
    EXPECT_TRUE(clock.outside());
    clock.report(false);
    EXPECT_FALSE(clock.outside());
    EXPECT_TRUE(capture.messages().back().ends_with(": open"));
    now.advance(8 * 3600);  // 20:00
    EXPECT_TRUE(clock.outside());
    SessionClock unknown(Hours {14400, 72000, "Nowhere/Nothing"}, std::nullopt, now.fn());
    EXPECT_TRUE(unknown.outside());  // no zone and no word from Arcus: closed, to be safe
    unknown.report(false);
    EXPECT_FALSE(unknown.outside());
}

TEST(Session, AnOvernightSessionRunsIntoTheNextMorning) {
    // 20:00 to 04:00 New York, opening Monday to Friday evening
    SessionClock clock(Hours {72000, 14400, "America/New_York", true});
    EXPECT_EQ(clock.by_clock(at(2026, 10, 19, 21, 0)), true);  // Monday evening
    EXPECT_EQ(clock.by_clock(at(2026, 10, 20, 3, 0)), true);  // Tuesday small hours
    EXPECT_EQ(clock.by_clock(at(2026, 10, 19, 3, 0)), false);  // Monday small hours: the weekend's
    EXPECT_EQ(clock.by_clock(at(2026, 10, 17, 3, 0)), true);  // Saturday small hours: Friday's session
    EXPECT_EQ(clock.by_clock(at(2026, 10, 17, 21, 0)), false);  // Saturday evening
    EXPECT_EQ(clock.by_clock(at(2026, 10, 19, 12, 0)), false);  // Monday noon
}
