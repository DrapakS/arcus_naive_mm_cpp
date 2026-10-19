#include "trading/session.hpp"

#include <cmath>
#include <format>

#include "util/log.hpp"

namespace nmm {

namespace {
Logger& log() { return get_logger(ORDERS_LOGGER); }
}  // namespace

std::optional<Hours> Hours::from_json(const Json* data) {
    if (data == nullptr || !data->is_object()) return std::nullopt;
    const Json* start = jget(*data, "startSecondsOfDay");
    const Json* end = jget(*data, "endSecondsOfDay");
    if (start == nullptr || start->is_null() || end == nullptr || end->is_null()) return std::nullopt;
    Hours h;
    h.start_s = static_cast<int>(to_int(*start));
    h.end_s = static_cast<int>(to_int(*end));
    const Json* tz = jget(*data, "timezone");
    h.tz = truthy(tz) ? to_str(*tz) : "UTC";
    h.overnight = truthy(jget(*data, "isOvernight"));
    return h;
}

std::string Hours::describe() const {
    auto hm = [](int s) { return std::format("{:02d}:{:02d}", s / 3600, s % 3600 / 60); };
    return std::format("{}-{} {}, Monday to Friday", hm(start_s), hm(end_s), tz);
}

SessionClock::SessionClock(Hours hours_, std::optional<bool> outside, Clock clock, std::string note)
    : hours(std::move(hours_)), note(std::move(note)), clock(std::move(clock)), reported_outside(outside) {
    try {
        zone_ = std::chrono::locate_zone(hours.tz);
    } catch (const std::exception&) {
        zone_ = nullptr;
        log().warning("!!! time zone {} unknown on this host: the session is told by Arcus's isOutsideRth alone",
                      hours.tz);
    }
}

std::optional<bool> SessionClock::by_clock(double now) {
    using namespace std::chrono;
    if (zone_ == nullptr) return std::nullopt;
    sys_seconds sys {seconds(static_cast<int64_t>(std::floor(now)))};
    auto local = zone_->to_local(sys);
    auto day = floor<days>(local);
    int s = static_cast<int>((local - day).count());
    int weekday = static_cast<int>(std::chrono::weekday(day).iso_encoding()) - 1;  // 0 = Monday
    const Hours& h = hours;
    if (!h.overnight) return weekday < 5 && h.start_s <= s && s < h.end_s;
    // overnight: from start on one weekday to end the next morning
    if (s >= h.start_s) return weekday < 5;
    return s < h.end_s && 0 < weekday && weekday <= 5;
}

bool SessionClock::outside() {
    double now = clock();
    if (cached_ && 0 <= now - cached_->first && now - cached_->first < 1.0) return cached_->second;  // a clock stepped back works it out again
    auto open = by_clock(now);
    bool closed = (open.has_value() && !*open) || (reported_outside.has_value() && *reported_outside)
                  || (!open.has_value() && !reported_outside.has_value());
    cached_ = std::make_pair(now, closed);
    if (!last_.has_value() || closed != *last_) {
        if (last_.has_value() || closed)
            log().info("--- trading session ({}): {}{}", hours.describe(), closed ? "closed" : "open", closed ? note : "");
        last_ = closed;
    }
    return closed;
}

void SessionClock::report(std::optional<bool> outside) {
    if (outside.has_value()) {
        reported_outside = outside;
        cached_.reset();
    }
}

}  // namespace nmm
