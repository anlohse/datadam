#include "util/strings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace datadam {

namespace {

char lower_char(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

template <class T>
std::optional<T> parse_floating(std::string_view s) {
  if (s.empty()) return std::nullopt;
  if (s.front() == '+') s.remove_prefix(1);
  if (s.empty()) return std::nullopt;
  T v{};
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
  if (std::isnan(v)) return std::nullopt;
  return v;
}

template <class T>
std::string format_floating(T v) {
  if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
  char buf[128];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  if (ec != std::errc()) return "0";
  return std::string(buf, ptr);
}

}  // namespace

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = lower_char(c);
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (lower_char(a[i]) != lower_char(b[i])) return false;
  }
  return true;
}

std::optional<int64_t> parse_int64(std::string_view s) {
  if (s.empty()) return std::nullopt;
  int64_t v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

std::optional<uint64_t> parse_uint64(std::string_view s) {
  if (s.empty() || s.front() == '-') return std::nullopt;
  uint64_t v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

std::optional<double> parse_double(std::string_view s) { return parse_floating<double>(s); }

std::optional<long double> parse_long_double(std::string_view s) {
  return parse_floating<long double>(s);
}

std::string format_double(double v) { return format_floating(v); }

std::string format_long_double(long double v) { return format_floating(v); }

std::optional<uint64_t> parse_size(std::string_view s) {
  size_t digits = 0;
  while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits]))) ++digits;
  if (digits == 0) return std::nullopt;
  auto base = parse_uint64(s.substr(0, digits));
  if (!base) return std::nullopt;
  std::string unit = to_lower(s.substr(digits));
  uint64_t mul = 1;
  if (unit.empty() || unit == "b") mul = 1;
  else if (unit == "k" || unit == "kb") mul = 1ULL << 10;
  else if (unit == "m" || unit == "mb") mul = 1ULL << 20;
  else if (unit == "g" || unit == "gb") mul = 1ULL << 30;
  else if (unit == "t" || unit == "tb") mul = 1ULL << 40;
  else return std::nullopt;
  if (*base > UINT64_MAX / mul) return std::nullopt;
  return *base * mul;
}

std::optional<std::chrono::milliseconds> parse_duration(std::string_view s) {
  size_t digits = 0;
  while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits]))) ++digits;
  if (digits == 0) return std::nullopt;
  auto base = parse_uint64(s.substr(0, digits));
  if (!base) return std::nullopt;
  std::string unit = to_lower(s.substr(digits));
  uint64_t mul;
  if (unit == "ms") mul = 1;
  else if (unit.empty() || unit == "s") mul = 1000;
  else if (unit == "m" || unit == "min") mul = 60'000;
  else if (unit == "h") mul = 3'600'000;
  else if (unit == "d") mul = 86'400'000;
  else return std::nullopt;
  if (*base > static_cast<uint64_t>(INT64_MAX) / mul) return std::nullopt;
  return std::chrono::milliseconds(static_cast<int64_t>(*base * mul));
}

std::string format_size(uint64_t bytes) {
  static constexpr const char* kUnits[] = {"b", "kb", "mb", "gb", "tb"};
  int unit = 0;
  while (unit < 4 && bytes >= 1024 && bytes % 1024 == 0) {
    bytes /= 1024;
    ++unit;
  }
  return std::to_string(bytes) + kUnits[unit];
}

namespace {

bool chars_equal(char a, char b, bool nocase) {
  return nocase ? lower_char(a) == lower_char(b) : a == b;
}

// Matches a single pattern token at p[pi] against c and advances pi past it.
bool match_token(std::string_view p, size_t& pi, char c, bool nocase) {
  char pc = p[pi];
  if (pc == '?') {
    ++pi;
    return true;
  }
  if (pc == '[') {
    size_t i = pi + 1;
    bool negate = false;
    if (i < p.size() && p[i] == '^') {
      negate = true;
      ++i;
    }
    bool matched = false;
    while (i < p.size() && p[i] != ']') {
      if (p[i] == '\\' && i + 1 < p.size()) {
        if (chars_equal(p[i + 1], c, nocase)) matched = true;
        i += 2;
      } else if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']') {
        char lo = p[i], hi = p[i + 2];
        if (lo > hi) std::swap(lo, hi);
        char cc = c;
        if (nocase) {
          lo = lower_char(lo);
          hi = lower_char(hi);
          cc = lower_char(c);
        }
        if (cc >= lo && cc <= hi) matched = true;
        i += 3;
      } else {
        if (chars_equal(p[i], c, nocase)) matched = true;
        ++i;
      }
    }
    pi = i < p.size() ? i + 1 : i;
    return matched != negate;
  }
  if (pc == '\\' && pi + 1 < p.size()) ++pi;
  bool r = chars_equal(p[pi], c, nocase);
  ++pi;
  return r;
}

}  // namespace

bool glob_match(std::string_view p, std::string_view s, bool nocase) {
  constexpr size_t kNone = std::string_view::npos;
  size_t pi = 0, si = 0, star_pi = kNone, star_si = 0;
  while (si < s.size()) {
    if (pi < p.size() && p[pi] == '*') {
      while (pi < p.size() && p[pi] == '*') ++pi;
      if (pi == p.size()) return true;
      star_pi = pi;
      star_si = si;
      continue;
    }
    if (pi < p.size()) {
      size_t next = pi;
      if (match_token(p, next, s[si], nocase)) {
        pi = next;
        ++si;
        continue;
      }
    }
    if (star_pi != kNone) {
      pi = star_pi;
      si = ++star_si;
      continue;
    }
    return false;
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

std::vector<std::string_view> split_ws(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

}  // namespace datadam
