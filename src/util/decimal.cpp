#include "util/decimal.hpp"

#include <cstdlib>

#include "util/pyfmt.hpp"

namespace nmm {

namespace {

using u128 = unsigned __int128;
using u64 = uint64_t;

constexpr u64 ONE64 = 1'000'000'000'000'000'000ULL;
constexpr u128 MAX_MAG = (static_cast<u128>(1) << 127) - 1;

u128 pow10(int n) {
    u128 r = 1;
    for (int i = 0; i < n; ++i) r *= 10;
    return r;
}

struct U256 {
    u128 hi = 0, lo = 0;
};

U256 mul(u128 a, u128 b) {
    u64 a0 = static_cast<u64>(a), a1 = static_cast<u64>(a >> 64);
    u64 b0 = static_cast<u64>(b), b1 = static_cast<u64>(b >> 64);
    u128 p0 = static_cast<u128>(a0) * b0;
    u128 p1 = static_cast<u128>(a0) * b1;
    u128 p2 = static_cast<u128>(a1) * b0;
    u128 p3 = static_cast<u128>(a1) * b1;
    U256 r;
    r.lo = p0;
    r.hi = p3;
    u128 add1 = p1 << 64;
    r.lo += add1;
    if (r.lo < add1) ++r.hi;
    u128 add2 = p2 << 64;
    r.lo += add2;
    if (r.lo < add2) ++r.hi;
    r.hi += (p1 >> 64) + (p2 >> 64);
    return r;
}

// round_half_even(n / d) for a 256-bit n and a 64-bit d; throws when it does not fit.
u128 div_round(const U256& n, u64 d) {
    u64 limbs[4] = {static_cast<u64>(n.hi >> 64), static_cast<u64>(n.hi), static_cast<u64>(n.lo >> 64),
                    static_cast<u64>(n.lo)};
    u64 q[4];
    u128 rem = 0;
    for (int i = 0; i < 4; ++i) {
        u128 cur = (rem << 64) | limbs[i];
        q[i] = static_cast<u64>(cur / d);
        rem = cur % d;
    }
    if (q[0] || q[1]) throw std::overflow_error("Decimal overflow");
    u128 quotient = (static_cast<u128>(q[2]) << 64) | q[3];
    u128 twice = rem * 2;
    if (twice > d || (twice == d && (quotient & 1))) ++quotient;
    return quotient;
}

// round_half_even(n / d) for a 256-bit n and a 128-bit d (< 2^127).
u128 div_round(const U256& n, u128 d) {
    u128 q_hi = 0, q_lo = 0, rem = 0;
    for (int bit = 255; bit >= 0; --bit) {
        u128 b = bit >= 128 ? (n.hi >> (bit - 128)) & 1 : (n.lo >> bit) & 1;
        rem = (rem << 1) | b;
        bool set = rem >= d;
        if (set) rem -= d;
        if (bit >= 128) {
            if (set) q_hi |= static_cast<u128>(1) << (bit - 128);
        } else if (set) {
            q_lo |= static_cast<u128>(1) << bit;
        }
    }
    if (q_hi) throw std::overflow_error("Decimal overflow");
    u128 twice = rem * 2;
    if (twice > d || (twice == d && (q_lo & 1))) ++q_lo;
    return q_lo;
}

__int128 signed_result(u128 magnitude, bool negative) {
    if (magnitude > MAX_MAG) throw std::overflow_error("Decimal overflow");
    __int128 v = static_cast<__int128>(magnitude);
    return negative ? -v : v;
}

u128 magnitude(__int128 v) { return v < 0 ? static_cast<u128>(-v) : static_cast<u128>(v); }

std::string u128_str(u128 v) {
    if (v == 0) return "0";
    std::string s;
    while (v) {
        s.insert(s.begin(), static_cast<char>('0' + static_cast<int>(v % 10)));
        v /= 10;
    }
    return s;
}

}  // namespace

Decimal operator*(const Decimal& a, const Decimal& b) {
    bool negative = (a.v_ < 0) != (b.v_ < 0);
    U256 product = mul(magnitude(a.v_), magnitude(b.v_));
    return Decimal::from_raw(signed_result(div_round(product, ONE64), negative));
}

Decimal operator/(const Decimal& a, const Decimal& b) {
    if (b.v_ == 0) throw std::domain_error("Decimal division by zero");
    bool negative = (a.v_ < 0) != (b.v_ < 0);
    U256 scaled = mul(magnitude(a.v_), static_cast<u128>(ONE64));
    return Decimal::from_raw(signed_result(div_round(scaled, magnitude(b.v_)), negative));
}

Decimal Decimal::parse(std::string_view text) {
    auto parsed = try_parse(text);
    if (!parsed) throw std::invalid_argument("not a decimal number: " + std::string(text));
    return *parsed;
}

std::optional<Decimal> Decimal::try_parse(std::string_view text) {
    std::string_view s = strip(text);
    if (s.empty()) return std::nullopt;
    bool negative = false;
    if (s[0] == '+' || s[0] == '-') {
        negative = s[0] == '-';
        s.remove_prefix(1);
    }
    std::string digits;
    int frac_len = 0;
    bool dot = false, any = false;
    size_t i = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            digits += c;
            any = true;
            if (dot) ++frac_len;
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            break;
        }
    }
    if (!any) return std::nullopt;
    int exp = 0;
    if (i < s.size()) {
        if (s[i] != 'e' && s[i] != 'E') return std::nullopt;
        ++i;
        bool exp_negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            exp_negative = s[i] == '-';
            ++i;
        }
        if (i >= s.size()) return std::nullopt;
        long e = 0;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return std::nullopt;
            e = e * 10 + (s[i] - '0');
            if (e > 10000) return std::nullopt;
        }
        exp = static_cast<int>(exp_negative ? -e : e);
    }
    size_t lead = digits.find_first_not_of('0');
    digits = lead == std::string::npos ? "" : digits.substr(lead);
    if (digits.empty()) return Decimal();
    int shift = exp - frac_len + SCALE;  // value * 10^18 = int(digits) * 10^shift
    u128 mag = 0;
    if (shift >= 0) {
        if (static_cast<int>(digits.size()) + shift > 38) return std::nullopt;
        for (char c : digits) mag = mag * 10 + static_cast<u128>(c - '0');
        mag *= pow10(shift);
    } else {
        int drop = -shift;
        if (drop > static_cast<int>(digits.size())) {
            return Decimal();  // below the last place, rounds to zero (half of it at most: still zero)
        }
        std::string kept = digits.substr(0, digits.size() - static_cast<size_t>(drop));
        std::string gone = digits.substr(digits.size() - static_cast<size_t>(drop));
        if (kept.size() > 38) return std::nullopt;
        for (char c : kept) mag = mag * 10 + static_cast<u128>(c - '0');
        // half to even on what was dropped
        int cmp = gone[0] > '5' ? 1 : gone[0] < '5' ? -1 : (gone.find_first_not_of('0', 1) != std::string::npos ? 1 : 0);
        if (cmp > 0 || (cmp == 0 && (mag & 1))) ++mag;
    }
    if (mag > MAX_MAG) return std::nullopt;
    __int128 v = static_cast<__int128>(mag);
    return from_raw(negative ? -v : v);
}

Decimal Decimal::from_double(double x) {
    std::string text = pyrepr(x);
    auto parsed = try_parse(text);
    if (!parsed) throw std::invalid_argument("cannot convert float " + text + " to Decimal");
    return *parsed;
}

double Decimal::to_double() const { return std::strtod(str().c_str(), nullptr); }

std::string Decimal::str() const {
    u128 mag = magnitude(v_);
    u128 whole = mag / ONE64;
    u128 frac = mag % ONE64;
    std::string out = v_ < 0 ? "-" : "";
    out += u128_str(whole);
    if (frac) {
        std::string f = u128_str(frac);
        f.insert(f.begin(), static_cast<size_t>(SCALE) - f.size(), '0');
        out += "." + std::string(rstrip(f, '0'));
    }
    return out;
}

std::string Decimal::fixed(int places, bool plus) const {
    if (places < 0) places = 0;
    if (places > SCALE) places = SCALE;
    u128 mag = magnitude(v_);
    u128 unit = pow10(SCALE - places);
    u128 q = mag / unit, r = mag % unit;
    if (unit > 1) {
        u128 twice = r * 2;
        if (twice > unit || (twice == unit && (q & 1))) ++q;
    }
    u128 base = pow10(places);
    std::string out = u128_str(q / base);
    if (places) {
        std::string f = u128_str(q % base);
        f.insert(f.begin(), static_cast<size_t>(places) - f.size(), '0');
        out += "." + f;
    }
    // Python keeps the sign of a negative that rounds to zero: "-0.00"
    if (v_ < 0) out = "-" + out;
    else if (plus) out = "+" + out;
    return out;
}

Decimal Decimal::floor_to(const Decimal& unit) const {
    if (unit.v_ <= 0) throw std::invalid_argument("floor_to needs a unit above zero");
    __int128 q = v_ / unit.v_;
    if (v_ % unit.v_ != 0 && v_ < 0) --q;
    return from_raw(q * unit.v_);
}

Decimal Decimal::ceil_to(const Decimal& unit) const {
    if (unit.v_ <= 0) throw std::invalid_argument("ceil_to needs a unit above zero");
    __int128 q = v_ / unit.v_;
    if (v_ % unit.v_ != 0 && v_ > 0) ++q;
    return from_raw(q * unit.v_);
}

int64_t Decimal::units(const Decimal& unit) const {
    if (unit.v_ == 0 || v_ % unit.v_ != 0)
        throw std::invalid_argument(str() + " is not a multiple of " + unit.str());
    return static_cast<int64_t>(v_ / unit.v_);
}

int64_t Decimal::round_int() const {
    u128 mag = magnitude(v_);
    u128 q = mag / ONE64, r = mag % ONE64;
    u128 twice = r * 2;
    if (twice > ONE64 || (twice == ONE64 && (q & 1))) ++q;
    int64_t out = static_cast<int64_t>(q);
    return v_ < 0 ? -out : out;
}

}  // namespace nmm
