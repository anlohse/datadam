#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace datadam {

std::string to_lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);

// Strict parsers: the whole input must be consumed, no surrounding spaces.
std::optional<int64_t> parse_int64(std::string_view s);
std::optional<uint64_t> parse_uint64(std::string_view s);
// Accepts "inf", "+inf" and "-inf"; rejects NaN.
std::optional<double> parse_double(std::string_view s);
std::optional<long double> parse_long_double(std::string_view s);

// Shortest representation that round-trips.
std::string format_double(double v);
std::string format_long_double(long double v);

// "4096", "4kb", "512mb", "1g" (binary units, case-insensitive).
std::optional<uint64_t> parse_size(std::string_view s);
// "500ms", "30s", "5m", "1h", "1d"; a bare number means seconds. "0" disables.
std::optional<std::chrono::milliseconds> parse_duration(std::string_view s);

std::string format_size(uint64_t bytes);

// Redis-style glob: *, ?, [abc], [^a-z], \x escapes.
bool glob_match(std::string_view pattern, std::string_view str, bool nocase = false);

std::vector<std::string_view> split_ws(std::string_view s);

}  // namespace datadam
