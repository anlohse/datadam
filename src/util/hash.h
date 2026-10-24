#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace datadam {

inline uint64_t mix64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

// Fast non-cryptographic 64-bit hash for keys: 8-byte blocks folded with the
// murmur3 finalizer. Good distribution for hash tables and shard selection.
inline uint64_t hash_bytes(std::string_view s) {
  constexpr uint64_t kMul = 0x9E3779B97F4A7C15ULL;
  const char* p = s.data();
  size_t n = s.size();
  uint64_t h = 0xcbf29ce484222325ULL ^ (static_cast<uint64_t>(n) * kMul);
  while (n >= 8) {
    uint64_t k;
    std::memcpy(&k, p, 8);
    h = (h ^ mix64(k)) * kMul;
    p += 8;
    n -= 8;
  }
  if (n > 0) {
    uint64_t k = 0;
    std::memcpy(&k, p, n);
    h = (h ^ mix64(k)) * kMul;
  }
  return mix64(h);
}

}  // namespace datadam
