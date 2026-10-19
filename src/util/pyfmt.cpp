#include "util/pyfmt.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <sys/resource.h>
#include <sys/time.h>

#include "util/clock.hpp"

namespace nmm {

double process_time() {
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    auto seconds = [](const timeval& tv) { return static_cast<double>(tv.tv_sec) + tv.tv_usec / 1e6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

std::string pyrepr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    if (x == 0.0) return std::signbit(x) ? "-0.0" : "0.0";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    bool negative = sci[0] == '-';
    if (negative) sci.erase(0, 1);
    auto epos = sci.find('e');
    std::string mantissa = sci.substr(0, epos);
    int exp10 = std::stoi(sci.substr(epos + 1));
    std::string digits;
    for (char c : mantissa)
        if (c != '.') digits += c;
    int decpt = exp10 + 1;  // value = 0.digits * 10^decpt
    std::string out = negative ? "-" : "";
    if (-4 < decpt && decpt <= 16) {
        if (decpt <= 0) {
            out += "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
        } else if (static_cast<size_t>(decpt) >= digits.size()) {
            out += digits + std::string(static_cast<size_t>(decpt) - digits.size(), '0') + ".0";
        } else {
            out += digits.substr(0, static_cast<size_t>(decpt)) + "." + digits.substr(static_cast<size_t>(decpt));
        }
    } else {
        out += digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        int e = decpt - 1;
        out += std::format("e{}{:02d}", e < 0 ? '-' : '+', std::abs(e));
    }
    return out;
}

std::string pyrepr(std::optional<double> x) { return x ? pyrepr(*x) : "None"; }

std::string pyrepr(std::string_view s) {
    bool has_single = s.find('\'') != std::string_view::npos;
    bool has_double = s.find('"') != std::string_view::npos;
    char quote = has_single && !has_double ? '"' : '\'';
    std::string out(1, quote);
    for (char c : s) {
        if (c == quote || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    out += quote;
    return out;
}

std::string thousands(double x, int places) {
    std::string plain = std::format("{:.{}f}", std::fabs(x), places);
    auto dot = plain.find('.');
    std::string whole = plain.substr(0, dot);
    std::string frac = dot == std::string::npos ? "" : plain.substr(dot);
    std::string grouped;
    int count = 0;
    for (auto it = whole.rbegin(); it != whole.rend(); ++it) {
        if (count && count % 3 == 0) grouped.insert(grouped.begin(), ',');
        grouped.insert(grouped.begin(), *it);
        ++count;
    }
    bool negative = x < 0 && plain.find_first_not_of("0.") != std::string::npos;
    return (negative ? "-" : "") + grouped + frac;
}

std::string percent(double share, int places) { return std::format("{:.{}f}%", share * 100.0, places); }

std::string local_time_us() {
    int64_t us = now_us();
    std::time_t secs = static_cast<std::time_t>(us / 1'000'000);
    std::tm tm {};
    localtime_r(&secs, &tm);
    return std::format("{:02d}:{:02d}:{:02d}.{:06d}", tm.tm_hour, tm.tm_min, tm.tm_sec, us % 1'000'000);
}

std::string utc_time_us(int64_t us) {
    int64_t secs = us >= 0 ? us / 1'000'000 : (us - 999'999) / 1'000'000;
    int64_t frac = us - secs * 1'000'000;
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm {};
    gmtime_r(&t, &tm);
    return std::format("{:02d}:{:02d}:{:02d}.{:06d}", tm.tm_hour, tm.tm_min, tm.tm_sec, frac);
}

std::string utc_strftime(double unix_s, const char* format) {
    std::time_t t = static_cast<std::time_t>(std::floor(unix_s));
    std::tm tm {};
    gmtime_r(&t, &tm);
    char buf[128];
    size_t n = std::strftime(buf, sizeof buf, format, &tm);
    return std::string(buf, n);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view strip(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

std::string_view remove_suffix(std::string_view s, std::string_view suffix) {
    if (!suffix.empty() && ends_with(s, suffix)) s.remove_suffix(suffix.size());
    return s;
}

std::string_view remove_prefix(std::string_view s, std::string_view prefix) {
    if (!prefix.empty() && starts_with(s, prefix)) s.remove_prefix(prefix.size());
    return s;
}

std::string_view rstrip(std::string_view s, char c) {
    while (!s.empty() && s.back() == c) s.remove_suffix(1);
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) ++i;
        size_t start = i;
        while (i < s.size() && !is_space(s[i])) ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    for (size_t start = 0;;) {
        size_t at = s.find(sep, start);
        out.emplace_back(s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) return out;
        start = at + 1;
    }
}

std::string to_hex(const unsigned char* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

}  // namespace nmm
