// Python-style formatting the log lines rely on: repr() of a float, the
// time-of-day stamps, and a few format specs std::format lacks.
#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nmm {

// repr(float) in Python: the shortest string that reads back to the same
// double, "83121.0" for a whole number, "1e-05" / "1e+16" outside 1e-4..1e16.
std::string pyrepr(double x);

// str() of an optional float: "None" when empty.
std::string pyrepr(std::optional<double> x);

// A Python string literal as repr() writes it: 'BTCUSDT'.
std::string pyrepr(std::string_view s);

// format(x, ",.0f"): thousands separated by commas.
std::string thousands(double x, int places = 0);

// format(x, ".N%"): a share as a percentage.
std::string percent(double share, int places = 0);

// Local time of day "HH:MM:SS.ffffff" (datetime.now().strftime("%H:%M:%S.%f")).
std::string local_time_us();

// Unix µs as UTC time of day "HH:MM:SS.ffffff".
std::string utc_time_us(int64_t us);

// Unix seconds as UTC with a strftime format (seconds precision).
std::string utc_strftime(double unix_s, const char* format);

// Lower / upper case (ASCII).
std::string lower(std::string_view s);
std::string upper(std::string_view s);

// str.strip / rstrip of whitespace, and of a suffix / prefix.
std::string_view strip(std::string_view s);
std::string_view remove_suffix(std::string_view s, std::string_view suffix);
std::string_view remove_prefix(std::string_view s, std::string_view prefix);
std::string_view rstrip(std::string_view s, char c);

bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);

// Join with a separator.
std::string join(const std::vector<std::string>& parts, std::string_view sep);

// Python's str.split() with no argument: on runs of whitespace.
std::vector<std::string> split_ws(std::string_view s);

// Hex of bytes, lower case.
std::string to_hex(const unsigned char* data, size_t n);

}  // namespace nmm
