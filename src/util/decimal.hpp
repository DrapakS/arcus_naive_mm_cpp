// Exact decimal arithmetic for prices and sizes, in place of Python's Decimal.
//
// A fixed-point number with 18 decimal places in a 128-bit integer: every price
// and size an exchange sends (and their products, size x price) is exact, the
// sums of a position never drift, and a quotient is rounded half to even at the
// 18th place. Range: about +-1.7e20, far beyond any dollar amount the bot sees.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace nmm {

class Decimal {
public:
    static constexpr int SCALE = 18;

    constexpr Decimal() = default;
    template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
    constexpr Decimal(I value) : v_(static_cast<__int128>(value) * ONE) {}  // NOLINT: implicit, as Decimal(0)
    explicit Decimal(std::string_view text) : Decimal(parse(text)) {}
    explicit Decimal(const char* text) : Decimal(parse(text)) {}

    // "83191.5", "-0.0001202", "1e-05", "1.5E+3"; throws std::invalid_argument.
    static Decimal parse(std::string_view text);
    static std::optional<Decimal> try_parse(std::string_view text);
    // Decimal(str(x)): the shortest repr of the float, exactly.
    static Decimal from_double(double x);

    double to_double() const;
    // Plain decimal without exponent or trailing zeros ("83191.5", "0.001", "100").
    std::string str() const;
    // Fixed places, rounded half to even (format(d, ".2f")); `plus` puts "+" on a positive.
    std::string fixed(int places, bool plus = false) const;

    // Rounded down / up to a multiple of `unit` (> 0).
    Decimal floor_to(const Decimal& unit) const;
    Decimal ceil_to(const Decimal& unit) const;
    // How many `unit`s this is; throws unless an exact multiple.
    int64_t units(const Decimal& unit) const;
    // Rounded half to even to a whole number.
    int64_t round_int() const;

    bool is_zero() const { return v_ == 0; }
    explicit operator bool() const { return v_ != 0; }
    int sign() const { return v_ > 0 ? 1 : v_ < 0 ? -1 : 0; }
    Decimal abs() const { return from_raw(v_ < 0 ? -v_ : v_); }

    Decimal operator-() const { return from_raw(-v_); }
    friend Decimal operator+(const Decimal& a, const Decimal& b) { return from_raw(a.v_ + b.v_); }
    friend Decimal operator-(const Decimal& a, const Decimal& b) { return from_raw(a.v_ - b.v_); }
    friend Decimal operator*(const Decimal& a, const Decimal& b);
    friend Decimal operator/(const Decimal& a, const Decimal& b);
    Decimal& operator+=(const Decimal& o) { v_ += o.v_; return *this; }
    Decimal& operator-=(const Decimal& o) { v_ -= o.v_; return *this; }
    Decimal& operator*=(const Decimal& o) { return *this = *this * o; }
    Decimal& operator/=(const Decimal& o) { return *this = *this / o; }

    friend bool operator==(const Decimal& a, const Decimal& b) { return a.v_ == b.v_; }
    friend std::strong_ordering operator<=>(const Decimal& a, const Decimal& b) {
        return a.v_ < b.v_ ? std::strong_ordering::less
               : a.v_ > b.v_ ? std::strong_ordering::greater
                             : std::strong_ordering::equal;
    }

    __int128 raw() const { return v_; }
    static Decimal from_raw(__int128 v) {
        Decimal d;
        d.v_ = v;
        return d;
    }

private:
    static constexpr __int128 ONE = static_cast<__int128>(1'000'000'000'000'000'000LL);
    __int128 v_ = 0;
};

inline Decimal abs(const Decimal& d) { return d.abs(); }
inline Decimal min(const Decimal& a, const Decimal& b) { return b < a ? b : a; }
inline Decimal max(const Decimal& a, const Decimal& b) { return a < b ? b : a; }

// The market's fmt(): plain decimal, no exponent, no trailing zeros.
inline std::string fmt(const Decimal& d) { return d.str(); }

}  // namespace nmm
