#include "util/json.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "util/pyfmt.hpp"

namespace nmm {

Json parse_json(std::string_view text) {
    boost::system::error_code ec;
    Json v = boost::json::parse(text, ec);
    if (ec) throw std::runtime_error("malformed JSON: " + ec.message());
    return v;
}

FrameParser::FrameParser(size_t buffer_bytes) : buffer_(buffer_bytes) {}

const Json& FrameParser::parse(std::string_view text) {
    value_.reset();  // lets go of the last frame's memory before the buffer is reused
    resource_.reset();
    resource_.emplace(buffer_.data(), buffer_.size());
    parser_.reset(boost::json::storage_ptr(&*resource_));
    boost::system::error_code ec;
    parser_.write(text.data(), text.size(), ec);
    if (ec) {
        parser_.reset();
        throw std::runtime_error("malformed JSON: " + ec.message());
    }
    value_.emplace(parser_.release());
    return *value_;
}

const Json* jget(const Json& v, std::string_view key) {
    const auto* obj = v.if_object();
    if (obj == nullptr) return nullptr;
    return obj->if_contains(key);
}

const Json* jget(const Json* v, std::string_view key) { return v == nullptr ? nullptr : jget(*v, key); }

bool truthy(const Json* v) {
    if (v == nullptr) return false;
    switch (v->kind()) {
    case boost::json::kind::null: return false;
    case boost::json::kind::bool_: return v->get_bool();
    case boost::json::kind::int64: return v->get_int64() != 0;
    case boost::json::kind::uint64: return v->get_uint64() != 0;
    case boost::json::kind::double_: return v->get_double() != 0.0;
    case boost::json::kind::string: return !v->get_string().empty();
    case boost::json::kind::array: return !v->get_array().empty();
    case boost::json::kind::object: return !v->get_object().empty();
    }
    return false;
}

bool is_number(const Json& v) { return v.is_int64() || v.is_uint64() || v.is_double(); }

double to_float(const Json& v) {
    switch (v.kind()) {
    case boost::json::kind::int64: return static_cast<double>(v.get_int64());
    case boost::json::kind::uint64: return static_cast<double>(v.get_uint64());
    case boost::json::kind::double_: return v.get_double();
    case boost::json::kind::bool_: return v.get_bool() ? 1.0 : 0.0;
    case boost::json::kind::string: {
        std::string_view s = strip(std::string_view(v.get_string()));
        if (!s.empty() && s.front() == '+') s.remove_prefix(1);  // float("+1.5"); from_chars takes no sign but "-"
        if (s.empty()) break;
        double x = 0.0;
        auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), x);
        if (ec == std::errc() && end == s.data() + s.size()) return x;
        // what from_chars does not read the way float() does: inf, nan and the like
        std::string text(s);
        char* stop = nullptr;
        x = std::strtod(text.c_str(), &stop);
        if (stop != text.c_str() + text.size()) break;
        return x;
    }
    default: break;
    }
    throw std::invalid_argument("could not convert to float: " + dump_json(v));
}

int64_t to_int(const Json& v) {
    switch (v.kind()) {
    case boost::json::kind::int64: return v.get_int64();
    case boost::json::kind::uint64: return static_cast<int64_t>(v.get_uint64());
    case boost::json::kind::double_: {
        double d = v.get_double();
        if (!std::isfinite(d)) break;
        return static_cast<int64_t>(d);
    }
    case boost::json::kind::bool_: return v.get_bool() ? 1 : 0;
    case boost::json::kind::string: {
        std::string s(strip(std::string_view(v.get_string())));
        if (s.empty()) break;
        size_t i = s[0] == '+' || s[0] == '-' ? 1 : 0;
        if (i == s.size() || s.find_first_not_of("0123456789", i) != std::string::npos) break;
        errno = 0;
        long long x = std::strtoll(s.c_str(), nullptr, 10);
        if (errno == ERANGE) break;
        return x;
    }
    default: break;
    }
    throw std::invalid_argument("invalid literal for int(): " + dump_json(v));
}

std::string to_str(const Json& v) {
    switch (v.kind()) {
    case boost::json::kind::null: return "None";
    case boost::json::kind::bool_: return v.get_bool() ? "True" : "False";
    case boost::json::kind::int64: return std::to_string(v.get_int64());
    case boost::json::kind::uint64: return std::to_string(v.get_uint64());
    case boost::json::kind::double_: return pyrepr(v.get_double());
    case boost::json::kind::string: return std::string(v.get_string());
    default: return dump_json(v);
    }
}

std::optional<std::string> jstr(const Json& v, std::string_view key) {
    const Json* f = jget(v, key);
    if (f == nullptr || !f->is_string()) return std::nullopt;
    return std::string(f->get_string());
}

std::string jstr_or(const Json& v, std::string_view key, std::string_view fallback) {
    const Json* f = jget(v, key);
    return f == nullptr ? std::string(fallback) : to_str(*f);
}

std::string dump_json(const Json& v) { return boost::json::serialize(v); }

namespace {

void ascii_string(std::string& out, std::string_view s) {
    out += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                out += std::format("\\u{:04x}", c);
            } else if (c < 0x80) {
                out += static_cast<char>(c);
            } else {
                // UTF-8 to \uXXXX (with surrogate pairs), as ensure_ascii does
                uint32_t cp = 0;
                int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
                cp = c & (0x3F >> extra);
                for (int k = 0; k < extra && i + 1 < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[++i]) & 0x3F);
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    out += std::format("\\u{:04x}\\u{:04x}", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
                } else {
                    out += std::format("\\u{:04x}", cp);
                }
            }
        }
    }
    out += '"';
}

void canonical(std::string& out, const Json& v) {
    switch (v.kind()) {
    case boost::json::kind::object: {
        const auto& obj = v.get_object();
        std::vector<std::string_view> keys;
        for (const auto& kv : obj) keys.push_back(kv.key());
        std::sort(keys.begin(), keys.end());
        out += '{';
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) out += ',';
            ascii_string(out, keys[i]);
            out += ':';
            canonical(out, obj.at(keys[i]));
        }
        out += '}';
        break;
    }
    case boost::json::kind::array: {
        out += '[';
        const auto& arr = v.get_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) out += ',';
            canonical(out, arr[i]);
        }
        out += ']';
        break;
    }
    case boost::json::kind::string: ascii_string(out, v.get_string()); break;
    case boost::json::kind::null: out += "null"; break;
    case boost::json::kind::bool_: out += v.get_bool() ? "true" : "false"; break;
    case boost::json::kind::int64: out += std::to_string(v.get_int64()); break;
    case boost::json::kind::uint64: out += std::to_string(v.get_uint64()); break;
    case boost::json::kind::double_: {
        double d = v.get_double();
        out += std::isfinite(d) ? pyrepr(d) : (std::isnan(d) ? "NaN" : (d > 0 ? "Infinity" : "-Infinity"));
        break;
    }
    }
}

}  // namespace

std::string canonical_json(const Json& v) {
    std::string out;
    canonical(out, v);
    return out;
}

}  // namespace nmm
