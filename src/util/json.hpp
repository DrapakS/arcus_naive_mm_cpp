// JSON over Boost.JSON, with the loose reads the bot does in Python: a level's
// price as a string or a number (float(x)), a sequence id as an int or a
// numeric string (int(x)), Python's truthiness of a field.
#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nmm {

using Json = boost::json::value;
using JsonObject = boost::json::object;
using JsonArray = boost::json::array;

// Parse a message; throws std::runtime_error on malformed JSON.
Json parse_json(std::string_view text);

// The field of an object; nullptr when `v` is no object or has no such key.
const Json* jget(const Json& v, std::string_view key);
const Json* jget(const Json* v, std::string_view key);

// Python truthiness: null, false, 0, "", [], {} and a missing field are false.
bool truthy(const Json* v);
inline bool truthy(const Json& v) { return truthy(&v); }

bool is_number(const Json& v);

// float(x): a number, or a string holding one; throws std::invalid_argument.
double to_float(const Json& v);
// int(x): an integer, a float (truncated), or a string holding an integer; throws std::invalid_argument.
int64_t to_int(const Json& v);
// str(x): a string as it is, anything else as Python prints it (True, None, 1.5, ...).
std::string to_str(const Json& v);
inline std::string to_str(const Json* v) { return v == nullptr ? "None" : to_str(*v); }

// The string at `key`, or nullopt when missing or not a string.
std::optional<std::string> jstr(const Json& v, std::string_view key);
// str(obj.get(key, default)) as Python would print it.
std::string jstr_or(const Json& v, std::string_view key, std::string_view fallback = "None");

// Compact JSON, keys in their order (orjson.dumps).
std::string dump_json(const Json& v);
// json.dumps(v, separators=(",", ":"), sort_keys=True): what Arcus signs.
std::string canonical_json(const Json& v);

}  // namespace nmm
