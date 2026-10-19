// Market opens: no adding to the position around the moment the underlying starts trading again.
//
// A perp on Arcus trades round the clock, its underlying does not. When the underlying opens (gold and
// the US index futures on Sunday 18:00 New York after the weekend, and daily after their 17:00-18:00
// break; US stocks at 04:00 and 09:30) its price moves to where the world now values it, and the feeds
// the bot prices from move with it, in a run of one direction. Quoting both sides through that, the
// bot sells into the whole move and holds the position (GLD, 18 Oct 22:03-22:08 UTC, the Sunday gold
// reopen: seven sells in a row, 4 GLD short, -31 bp, the bot's whole GLD loss of the night).
//
// OpenWindows knows the opens (weekly, local time in a time zone, so daylight saving time is
// followed) and tells when now is within `before_s` before to `after_s` after one of them. The
// strategy then places no order that opens or adds to the position (or, with a limit share, none past
// that share of the limit); the order that shrinks it is quoted as usual. No taker, no stop-loss.
#pragma once

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/clock.hpp"

namespace nmm {

struct Open {
    std::set<int> days;  // weekdays it happens on, 0 = Monday
    int hour = 0;
    int minute = 0;
    std::string text;  // as the config wrote it

    // "Sun 18:00", "Mon-Thu 18:00", "Mon-Fri 09:30": a day or a range of days, then the local time.
    static Open parse(std::string_view text);
    bool operator==(const Open&) const = default;
};

// The host knows this time zone (zoneinfo).
bool known_time_zone(const std::string& tz);

// Unix seconds of `local_s` (seconds since the epoch of the local calendar) in `zone`; an
// ambiguous or skipped local time takes the offset in force before the change (Python's fold=0).
double local_to_unix(const std::chrono::time_zone* zone, int64_t local_s);

class OpenWindows {
public:
    OpenWindows(std::vector<std::string> opens, std::string tz, double before_s, double after_s,
                Clock clock = wall_time);

    std::string describe() const;
    // The open (and when it is, Unix seconds) whose window holds now; nullopt when none does.
    // Worked out once a second; the start and the end of a window are logged.
    std::optional<std::pair<Open, double>> window(std::optional<double> now = {});
    bool active(std::optional<double> now = {}) { return window(now).has_value(); }
    // The next open after `now` within a week, for the start-up line.
    std::optional<std::pair<Open, double>> next_open(std::optional<double> now = {});

    std::vector<Open> opens;
    std::string tz;
    double before_s;
    double after_s;
    Clock clock;
    // (when, what window() found then): reused for a second
    std::optional<std::pair<double, std::optional<std::pair<Open, double>>>> cached;

private:
    // Each open on the days around `now` (local), as Unix seconds.
    std::vector<std::pair<Open, double>> occurrences(double now) const;

    const std::chrono::time_zone* zone_;
    std::optional<double> current_;  // the open (Unix seconds) whose window holds now, as last logged
};

}  // namespace nmm
