// [open_guard]: no adding to the position around the moments the underlying opens (the windows
// themselves; the strategy and the config sides of tests/test_opens.py are elsewhere).
#include <gtest/gtest.h>

#include <ctime>

#include "test_util.hpp"
#include "trading/opens.hpp"
#include "util/log.hpp"

using nmm::Open;
using nmm::OpenWindows;

namespace {

double utc(int y, int mo, int d, int h, int mi, int s = 0) {
    std::tm tm {};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return static_cast<double>(timegm(&tm));
}

const double SUNDAY_REOPEN = utc(2026, 10, 18, 22, 0);  // Sunday 18:00 New York (EDT): the gold reopen of 18 Oct

struct Windows {
    std::shared_ptr<double> now;
    OpenWindows w;
    explicit Windows(double t, std::vector<std::string> opens = {"Sun 18:00", "Mon-Thu 18:00"}, double before_s = 60.0,
                     double after_s = 900.0)
        : now(std::make_shared<double>(t)),
          w(std::move(opens), "America/New_York", before_s, after_s, [p = now] { return *p; }) {}
};

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

TEST(Opens, AnOpenIsADayOrARangeOfDaysAndALocalTime) {
    EXPECT_EQ(Open::parse("Sun 18:00"), (Open {{6}, 18, 0, "Sun 18:00"}));
    EXPECT_EQ(Open::parse("mon-thu 18:00").days, (std::set<int> {0, 1, 2, 3}));
    EXPECT_EQ(Open::parse("Fri-Mon 9:30").days, (std::set<int> {4, 5, 6, 0}));  // wraps over the weekend
    for (const char* bad : {"Sunday 18:00", "Sun 25:00", "Sun", "18:00", "Sun 18:61", "Xyz 10:00"})
        EXPECT_THROW(Open::parse(bad), std::invalid_argument) << bad;
    try {
        Open::parse("Sunday 18:00");
    } catch (const std::invalid_argument& e) {
        EXPECT_STREQ(e.what(), "a market open is \"Day HH:MM\" or \"Day-Day HH:MM\", e.g. \"Sun 18:00\": 'Sunday 18:00'");
    }
}

TEST(Opens, TheWindowRunsFromBeforeSBeforeToAfterSAfterTheOpen) {
    Windows t(SUNDAY_REOPEN);
    for (auto [dt, inside] : std::vector<std::pair<double, bool>> {{-61, false}, {-60, true}, {0, true}, {900, true}, {901, false}}) {
        t.w.cached.reset();
        EXPECT_EQ(t.w.active(SUNDAY_REOPEN + dt), inside) << dt;
    }
    t.w.cached.reset();
    EXPECT_FALSE(t.w.active(utc(2026, 10, 16, 22, 0)));  // Friday: not a "Mon-Thu" day
    t.w.cached.reset();
    EXPECT_TRUE(t.w.active(utc(2026, 10, 19, 22, 5)));  // Monday 18:05 New York: after the daily break
}

TEST(Opens, TheOpensFollowDaylightSavingTime) {
    Windows t(0.0);
    double sunday_after_dst = utc(2026, 11, 8, 23, 0);  // 18:00 EST is 23:00 UTC from 1 Nov
    EXPECT_TRUE(t.w.active(sunday_after_dst));
    t.w.cached.reset();
    EXPECT_FALSE(t.w.active(utc(2026, 11, 8, 22, 0)));
}

TEST(Opens, TheStartAndTheEndOfAWindowAreLoggedAndTheNextOpenIsKnown) {
    Capture capture;
    Windows t(SUNDAY_REOPEN - 3600);
    EXPECT_FALSE(t.w.active());
    auto nxt = t.w.next_open();
    ASSERT_TRUE(nxt);
    EXPECT_EQ(nxt->first.text, "Sun 18:00");
    EXPECT_EQ(nxt->second, SUNDAY_REOPEN);
    *t.now = SUNDAY_REOPEN + 30;
    EXPECT_TRUE(t.w.active());
    auto messages = capture.messages();
    EXPECT_NE(std::find(messages.begin(), messages.end(),
                        "--- market open Sun 18:00 America/New_York (Sun 22:00 UTC): no adding to the position until "
                        "Sun 22:15 UTC; the closing order is quoted as usual"),
              messages.end());
    *t.now = SUNDAY_REOPEN + 1000;
    EXPECT_FALSE(t.w.active());
    EXPECT_EQ(capture.messages().back(), "--- market open window over: adding allowed again");
}

TEST(Opens, DescribeAndTheTimeZone) {
    Windows t(SUNDAY_REOPEN);
    EXPECT_EQ(t.w.describe(), "Sun 18:00, Mon-Thu 18:00 America/New_York: no adding from 60 s before to 900 s after each");
    EXPECT_TRUE(nmm::known_time_zone("America/New_York"));
    EXPECT_TRUE(nmm::known_time_zone("UTC"));
    EXPECT_FALSE(nmm::known_time_zone("Mars/Olympus"));
    EXPECT_THROW(OpenWindows({"Sun 18:00"}, "Mars/Olympus", 60, 900), std::invalid_argument);
}

TEST(Opens, ASkippedLocalTimeTakesTheOffsetBeforeTheChange) {
    // 8 Mar 2026, 02:30 New York does not exist (02:00 EST -> 03:00 EDT): Python's fold=0 reads it as EST
    const auto* zone = std::chrono::locate_zone("America/New_York");
    auto local = [](int y, int mo, int d, int h, int mi) {
        using namespace std::chrono;
        auto date = local_days {year {y} / month {static_cast<unsigned>(mo)} / std::chrono::day {static_cast<unsigned>(d)}};
        return date.time_since_epoch().count() * 86400LL + h * 3600 + mi * 60;
    };
    EXPECT_EQ(nmm::local_to_unix(zone, local(2026, 3, 8, 2, 30)), utc(2026, 3, 8, 7, 30));
    // 1 Nov 2026, 01:30 happens twice: the first (EDT)
    EXPECT_EQ(nmm::local_to_unix(zone, local(2026, 11, 1, 1, 30)), utc(2026, 11, 1, 5, 30));
}
