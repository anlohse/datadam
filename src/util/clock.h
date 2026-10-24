#pragma once

#include <chrono>
#include <cstdint>

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

}  // namespace datadam
