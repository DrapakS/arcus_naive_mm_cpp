// The trading session of a market with hours (Arcus RWA perps: stocks, ETFs): open or not.
//
// Arcus lists the hours as `regularTradingHours` (`startSecondsOfDay`, `endSecondsOfDay`,
// `timezone`, `isOvernight`) and says whether it is outside them now (`isOutsideRth`). For the
// US names the hours are the extended session, 04:00-20:00 New York, Monday to Friday. Outside it the
// underlying does not trade: the perp's price comes from the perp alone, within bounds around a
// settlement price, and a fill is more likely to be on the wrong side of the next open.
//
// SessionClock tells open from closed by the clock (weekdays in the market's time zone, exact at
// both ends) and by what Arcus last said (`isOutsideRth`, looked up every so often), which also
// knows the holidays: closed when either says so.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "util/clock.hpp"
#include "util/json.hpp"

namespace nmm {

struct Hours {
    int start_s = 0;  // seconds of the day the session opens, local time
    int end_s = 0;  // ... and closes
    std::string tz;
    bool overnight = false;  // opens one day and closes the next

    static std::optional<Hours> from_json(const Json* data);
    std::string describe() const;
    bool operator==(const Hours&) const = default;
};

class SessionClock {
public:
    SessionClock(Hours hours, std::optional<bool> outside = {}, Clock clock = wall_time, std::string note = "");

    // Open by the hours, Monday to Friday local time; nullopt without the time zone.
    std::optional<bool> by_clock(double now);
    // Closed now: by the clock or by what Arcus last said. Worked out once a second.
    bool outside();
    // What Arcus says now (isOutsideRth).
    void report(std::optional<bool> outside);

    Hours hours;
    std::string note;  // added to the "closed" line: what the bot does then
    Clock clock;
    std::optional<bool> reported_outside;  // Arcus's isOutsideRth, as last looked up

private:
    const std::chrono::time_zone* zone_ = nullptr;
    std::optional<std::pair<double, bool>> cached_;
    std::optional<bool> last_;
};

}  // namespace nmm
