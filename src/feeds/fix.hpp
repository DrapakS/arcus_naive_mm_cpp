// FIX 4.4 framing: messages to bytes and a byte stream back to messages (the
// collector's books/fix.py). Only what a market-data (QUOTE) session needs. A message
// is its (tag, value) pairs in wire order, so repeating groups keep their order.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nmm::fix {

inline constexpr char SOH = '\x01';
inline constexpr const char* BEGIN = "FIX.4.4";

// A malformed message (framing, length or checksum).
class FixError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Fields = std::vector<std::pair<int, std::string>>;

struct Message {
    Fields fields;

    std::string type() const { return get_or(35); }
    // The first value of `tag`, or nullptr.
    const std::string* get(int tag) const;
    std::string get_or(int tag, std::string fallback = "") const;
    // The entries of the repeating group counted by `count_tag`, each starting at `first_tag`
    // (in an entry, the first value of each tag).
    std::vector<std::map<int, std::string>> groups(int count_tag, int first_tag) const;
    // "35=W|34=7|...": for the logs, never the password (554).
    std::string str() const;
};

// The CheckSum (10) of `data`: the byte sum mod 256, three digits.
std::string checksum(std::string_view data);
// SendingTime: YYYYMMDD-HH:MM:SS.sss UTC.
std::string utc_timestamp(int64_t ts_us);
// YYYYMMDD-HH:MM:SS[.sss[sss]] UTC to epoch microseconds; nullopt if it does not parse.
std::optional<int64_t> parse_timestamp(std::string_view text);
// One message: BeginString, BodyLength, MsgType, then `header`, `body` and the CheckSum.
std::string encode(const std::string& msg_type, const Fields& header, const Fields& body);

// Bytes in, whole checked messages out.
class Parser {
public:
    // Every whole message `data` completes; throws FixError on a malformed one (the bytes
    // before the next "8=" are dropped).
    std::vector<Message> feed(std::string_view data);

private:
    std::optional<Message> next();
    std::string buf_;
};

}  // namespace nmm::fix
