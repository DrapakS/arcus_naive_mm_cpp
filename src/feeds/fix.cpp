#include "feeds/fix.hpp"

#include <charconv>
#include <ctime>
#include <format>

namespace nmm::fix {

namespace {

bool digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

int number(std::string_view s) {
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

}  // namespace

const std::string* Message::get(int tag) const {
    for (const auto& [t, v] : fields)
        if (t == tag) return &v;
    return nullptr;
}

std::string Message::get_or(int tag, std::string fallback) const {
    const std::string* v = get(tag);
    return v != nullptr ? *v : fallback;
}

std::vector<std::map<int, std::string>> Message::groups(int count_tag, int first_tag) const {
    std::vector<std::map<int, std::string>> out;
    bool inside = false;
    for (const auto& [t, v] : fields) {
        if (t == count_tag) {
            inside = true;
            continue;
        }
        if (!inside) continue;
        if (t == first_tag) out.emplace_back();
        if (!out.empty()) out.back().emplace(t, v);
    }
    return out;
}

std::string Message::str() const {
    std::string out;
    for (const auto& [t, v] : fields) {
        if (t == 554) continue;  // never the password
        if (!out.empty()) out += '|';
        out += std::format("{}={}", t, v);
    }
    return out;
}

std::string checksum(std::string_view data) {
    unsigned sum = 0;
    for (unsigned char c : data) sum += c;
    return std::format("{:03d}", sum % 256);
}

std::string utc_timestamp(int64_t ts_us) {
    std::time_t secs = static_cast<std::time_t>(ts_us / 1'000'000);
    std::tm t {};
    gmtime_r(&secs, &t);
    return std::format("{:04d}{:02d}{:02d}-{:02d}:{:02d}:{:02d}.{:03d}", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
                       t.tm_sec, static_cast<int>(ts_us % 1'000'000 / 1'000));
}

std::optional<int64_t> parse_timestamp(std::string_view text) {
    // YYYYMMDD-HH:MM:SS: 17 characters, then an optional fraction
    if (text.size() < 17 || text[8] != '-' || text[11] != ':' || text[14] != ':') return std::nullopt;
    for (auto [pos, len] : {std::pair<size_t, size_t> {0, 8}, {9, 2}, {12, 2}, {15, 2}})
        if (!digits(text.substr(pos, len))) return std::nullopt;
    std::tm t {};
    t.tm_year = number(text.substr(0, 4)) - 1900;
    t.tm_mon = number(text.substr(4, 2)) - 1;
    t.tm_mday = number(text.substr(6, 2));
    t.tm_hour = number(text.substr(9, 2));
    t.tm_min = number(text.substr(12, 2));
    t.tm_sec = number(text.substr(15, 2));
    if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31 || t.tm_hour > 23 || t.tm_min > 59 || t.tm_sec > 60)
        return std::nullopt;
    int64_t us = 0;
    if (text.size() > 17) {
        if (text[17] != '.') return std::nullopt;
        std::string frac(text.substr(18));
        if (!digits(frac)) return std::nullopt;
        frac = (frac + "000000").substr(0, 6);
        us = number(frac);
    }
    return static_cast<int64_t>(timegm(&t)) * 1'000'000 + us;
}

std::string encode(const std::string& msg_type, const Fields& header, const Fields& body) {
    std::string payload = std::format("35={}", msg_type);
    payload += SOH;
    for (const Fields* part : {&header, &body}) {
        for (const auto& [tag, value] : *part) {
            payload += std::format("{}={}", tag, value);
            payload += SOH;
        }
    }
    std::string data = std::format("8={}{}9={}{}", BEGIN, SOH, payload.size(), SOH) + payload;
    return data + "10=" + checksum(data) + SOH;
}

std::vector<Message> Parser::feed(std::string_view data) {
    buf_.append(data);
    std::vector<Message> out;
    while (auto msg = next()) out.push_back(std::move(*msg));
    return out;
}

std::optional<Message> Parser::next() {
    size_t start = buf_.find("8=");
    if (start == std::string::npos) {
        // keep a trailing "8" that may be the start of the next message
        buf_ = !buf_.empty() && buf_.back() == '8' ? "8" : "";
        return std::nullopt;
    }
    if (start) buf_.erase(0, start);
    size_t first = buf_.find(SOH);
    size_t second = first == std::string::npos ? std::string::npos : buf_.find(SOH, first + 1);
    if (second == std::string::npos) return std::nullopt;
    std::string_view length_field = std::string_view(buf_).substr(first + 1, second - first - 1);
    if (!length_field.starts_with("9=")) {
        std::string bad(length_field.substr(0, 20));
        buf_.erase(0, first + 1);
        throw FixError("no BodyLength after BeginString: " + bad);
    }
    if (!digits(length_field.substr(2))) {
        std::string bad(length_field);
        buf_.erase(0, second + 1);
        throw FixError("bad BodyLength " + bad);
    }
    size_t body_end = second + 1 + static_cast<size_t>(number(length_field.substr(2)));
    if (buf_.size() < body_end) return std::nullopt;
    size_t trailer_end = buf_.find(SOH, body_end);
    if (trailer_end == std::string::npos) return std::nullopt;
    std::string trailer = buf_.substr(body_end, trailer_end - body_end);
    std::string data = buf_.substr(0, body_end);
    buf_.erase(0, trailer_end + 1);
    if (!trailer.starts_with("10=") || trailer.substr(3) != checksum(data)) throw FixError("bad checksum " + trailer);
    Message msg;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find(SOH, pos);
        if (end == std::string::npos) end = data.size();
        std::string_view part = std::string_view(data).substr(pos, end - pos);
        pos = end + 1;
        if (part.empty()) continue;
        size_t eq = part.find('=');
        if (eq == std::string_view::npos || !digits(part.substr(0, eq))) throw FixError("bad field " + std::string(part.substr(0, 30)));
        msg.fields.emplace_back(number(part.substr(0, eq)), std::string(part.substr(eq + 1)));
    }
    return msg;
}

}  // namespace nmm::fix
