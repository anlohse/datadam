#pragma once

#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

namespace datadam {

// Keys of the data column family:
//   u32 key length | user key | u64 version | sub-key      (integers big endian)
// so all elements of one collection version are contiguous and sorted by sub-key.

inline void append_be64(std::string& out, uint64_t v) {
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

inline uint64_t read_be64(std::string_view s) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(s[i]);
  return v;
}

inline std::string data_prefix(std::string_view key, uint64_t version) {
  std::string out;
  out.reserve(4 + key.size() + 8);
  auto len = static_cast<uint32_t>(key.size());
  for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
  out.append(key);
  append_be64(out, version);
  return out;
}

// Splits a data key; false if it is malformed.
inline bool decode_data_key(std::string_view k, std::string_view* key, uint64_t* version, std::string_view* sub) {
  if (k.size() < 12) return false;
  uint32_t len = 0;
  for (size_t i = 0; i < 4; ++i) len = (len << 8) | static_cast<uint8_t>(k[i]);
  if (k.size() < 12 + static_cast<size_t>(len)) return false;
  *key = k.substr(4, len);
  *version = read_be64(k.substr(4 + len, 8));
  *sub = k.substr(12 + len);
  return true;
}

// u64 -> 8 bytes that sort like the number.
inline std::string encode_u64(uint64_t v) {
  std::string out;
  append_be64(out, v);
  return out;
}

// double -> u64 that sorts like the number (-inf < ... < -0 == 0 < ... < +inf).
inline uint64_t sortable_score(double d) {
  if (d == 0) d = 0;  // -0.0 -> +0.0
  uint64_t bits = std::bit_cast<uint64_t>(d);
  return (bits & (1ULL << 63)) ? ~bits : bits | (1ULL << 63);
}

inline double score_from_sortable(uint64_t v) {
  uint64_t bits = (v & (1ULL << 63)) ? v & ~(1ULL << 63) : ~v;
  return std::bit_cast<double>(bits);
}

}  // namespace datadam
