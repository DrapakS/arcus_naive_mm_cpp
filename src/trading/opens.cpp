#include "trading/opens.hpp"

#include <cmath>
#include <format>
#include <regex>
#include <stdexcept>

#include "util/log.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

const std::vector<std::string> DAYS {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

Logger& log() { return get_logger(ORDERS_LOGGER); }

int day_index(const std::string& day) {
    for (size_t i = 0; i < DAYS.size(); ++i)
        if (DAYS[i] == day) return static_cast<int>(i);
    return -1;
}

std::string utc(double t) { return utc_strftime(t, "%a %H:%M"); }

}  // namespace

Open Open::parse(std::string_view text) {
    static const std::regex OPEN_RE(R"(\s*([A-Za-z]{3})(?:\s*-\s*([A-Za-z]{3}))?\s+(\d{1,2}):(\d{2})\s*)");
    std::string s(text);
    std::smatch m;
    if (!std::regex_match(s, m, OPEN_RE))
        throw std::invalid_argument(
            "a market open is \"Day HH:MM\" or \"Day-Day HH:MM\", e.g. \"Sun 18:00\": " + pyrepr(text));
    std::string first = lower(m[1].str()), last = lower(m[2].matched ? m[2].str() : m[1].str());
    int a = day_index(first), b = day_index(last);
    if (a < 0 || b < 0)
        throw std::invalid_argument("unknown day in " + pyrepr(text) + ": one of Mon, Tue, Wed, Thu, Fri, Sat, Sun");
    int hour = std::stoi(m[3].str()), minute = std::stoi(m[4].str());
    if (hour > 23 || minute > 59) throw std::invalid_argument("no such time of day in " + pyrepr(text));
    Open o;
    if (a <= b) {
        for (int d = a; d <= b; ++d) o.days.insert(d);
    } else {  // Fri-Mon wraps
        for (int d = a; d < 7; ++d) o.days.insert(d);
        for (int d = 0; d <= b; ++d) o.days.insert(d);
    }
    o.hour = hour;
    o.minute = minute;
    o.text = std::string(strip(text));
    return o;
}

bool known_time_zone(const std::string& tz) {
    try {
        return tz.size() > 0 && std::chrono::locate_zone(tz) != nullptr;
    } catch (const std::exception&) {
        return false;
    }
}

double local_to_unix(const std::chrono::time_zone* zone, int64_t local_s) {
    using namespace std::chrono;
    local_seconds local {seconds(local_s)};
    local_info info = zone->get_info(local);
    // unique: its offset; ambiguous or skipped: the offset before the change (fold=0)
    return static_cast<double>(local_s - info.first.offset.count());
}

OpenWindows::OpenWindows(std::vector<std::string> opens_, std::string tz, double before_s, double after_s, Clock clock,
                         std::string holds, std::string closing, std::string over)
    : tz(std::move(tz)), before_s(before_s), after_s(after_s), clock(std::move(clock)), holds(std::move(holds)),
      closing(std::move(closing)), over(std::move(over)) {
    for (const auto& o : opens_) opens.push_back(Open::parse(o));
    try {
        zone_ = std::chrono::locate_zone(this->tz);
    } catch (const std::exception&) {
        throw std::invalid_argument("No time zone found with key " + this->tz);
    }
}

std::string OpenWindows::describe() const {
    std::vector<std::string> texts;
    for (const auto& o : opens) texts.push_back(o.text);
    return std::format("{} {}: {} from {:g} s before to {:g} s after each", join(texts, ", "), tz, holds, before_s,
                       after_s);
}

std::vector<std::pair<Open, double>> OpenWindows::occurrences(double now) const {
    using namespace std::chrono;
    sys_seconds sys {seconds(static_cast<int64_t>(std::floor(now)))};
    auto today = floor<days>(zone_->to_local(sys));
    std::vector<std::pair<Open, double>> out;
    for (int offset : {-1, 0, 1}) {
        auto day = today + days(offset);
        int wd = static_cast<int>(std::chrono::weekday(day).iso_encoding()) - 1;  // 0 = Monday
        for (const auto& o : opens) {
            if (!o.days.count(wd)) continue;
            int64_t local_s = day.time_since_epoch().count() * 86400 + o.hour * 3600 + o.minute * 60;
            out.emplace_back(o, local_to_unix(zone_, local_s));
        }
    }
    return out;
}

std::optional<std::pair<Open, double>> OpenWindows::window(std::optional<double> now_) {
    double now = now_ ? *now_ : clock();
    if (cached && 0 <= now - cached->first && now - cached->first < 1.0) return cached->second;
    std::optional<std::pair<Open, double>> found;
    for (auto& [o, at] : occurrences(now)) {
        if (at - before_s <= now && now <= at + after_s) {
            found = std::make_pair(o, at);
            break;
        }
    }
    cached = std::make_pair(now, found);
    std::optional<double> at = found ? std::optional<double>(found->second) : std::nullopt;
    if (at != current_) {
        if (found) {
            log().info("--- market open {} {} ({} UTC): {} until {} UTC{}", found->first.text, tz, utc(*at), holds,
                       utc(*at + after_s), closing.empty() ? "" : "; " + closing);
        } else {
            log().info("--- market open window over: {}", over);
        }
        current_ = at;
    }
    return found;
}

std::optional<std::pair<Open, double>> OpenWindows::next_open(std::optional<double> now_) {
    double now = now_ ? *now_ : clock();
    std::optional<std::pair<Open, double>> best;
    for (int d = 0; d < 8; ++d) {
        for (auto& [o, at] : occurrences(now + d * 86'400.0))
            if (at > now && (!best || at < best->second)) best = std::make_pair(o, at);
        if (best) return best;
    }
    return best;
}

}  // namespace nmm
