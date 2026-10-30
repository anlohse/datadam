#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace datadam {

// Wall-clock milliseconds since the Unix epoch. Used for TTLs, which are persisted.
inline int64_t unix_time_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Monotonic milliseconds. Used for cache bookkeeping (idle time, time in cache).
inline int64_t steady_time_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// now + ms, or "never" (INT64_MAX) when that would not fit: a client's
// timeout must not overflow into the past.
inline int64_t deadline_after(int64_t now_ms, int64_t ms) {
  constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
  return ms >= kNever - now_ms ? kNever : now_ms + ms;
}

}  // namespace datadam
