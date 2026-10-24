#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace datadam {

enum class ValueType : uint8_t {
  String = 0,
  Hash = 1,
  Set = 2,
  ZSet = 3,
  List = 4,
  Json = 5,  // one document, kept as compact JSON text (see server/cmd_json.cpp)
};

std::string_view value_type_name(ValueType t);
// Collections keep a CollectionMeta in the meta record and their elements in the data column family.
inline bool is_collection(ValueType t) {
  return t == ValueType::Hash || t == ValueType::Set || t == ValueType::ZSet || t == ValueType::List;
}

// Non-owning view of a stored object; what the cache hands out.
struct RecordView {
  ValueType type = ValueType::String;
  uint32_t flags = 0;         // memcache client flags
  int64_t expire_at_ms = 0;   // absolute Unix time in ms, 0 = no TTL
  uint64_t cas = 0;           // version, changes on every write
  std::string_view value;

  bool expired(int64_t now_ms) const { return expire_at_ms != 0 && now_ms >= expire_at_ms; }
};

struct Record {
  ValueType type = ValueType::String;
  uint32_t flags = 0;
  int64_t expire_at_ms = 0;
  uint64_t cas = 0;
  std::string value;

  static Record from(const RecordView& v) { return {v.type, v.flags, v.expire_at_ms, v.cas, std::string(v.value)}; }
  RecordView view() const { return {type, flags, expire_at_ms, cas, value}; }
  bool expired(int64_t now_ms) const { return expire_at_ms != 0 && now_ms >= expire_at_ms; }
};

// On-disk value layout (little endian):
//   u8 format | u8 type | u32 flags | i64 expire_at_ms | u64 cas | payload
constexpr uint8_t kRecordFormat = 1;
constexpr size_t kRecordHeaderSize = 1 + 1 + 4 + 8 + 8;

std::string encode_record(const RecordView& r);
// The returned view's `value` points into `raw`.
bool decode_record(std::string_view raw, RecordView* out);

// Payload of a collection's meta record. Every element key in the data column
// family carries `version`; a new collection always gets a new version, so the
// elements of a deleted or overwritten one can never be seen again, and they are
// removed lazily (range delete, or the data compaction filter).
struct CollectionMeta {
  static constexpr uint64_t kListStart = 1ULL << 63;

  uint64_t version = 0;
  uint64_t size = 0;
  uint64_t head = kListStart;  // list: index of the first element

  bool operator==(const CollectionMeta&) const = default;
};

constexpr size_t kCollectionMetaSize = 24;
std::string encode_collection_meta(const CollectionMeta& m);
// Reads the first kCollectionMetaSize bytes (a cached value may carry more, see below).
bool decode_collection_meta(std::string_view payload, CollectionMeta* out);

// In the object cache only (never on disk), the value of a small collection
// also carries all its elements, so reads and writes of small collections do
// not touch RocksDB:  meta | u8 ElementCache | (Complete: the elements, as
// encoded by ElementSet).
enum class ElementCache : uint8_t { Unknown = 0, Complete = 1, TooBig = 2 };
// `tail` is what follows the meta in a cached value.
inline ElementCache element_cache_state(std::string_view tail) {
  if (tail.empty()) return ElementCache::Unknown;
  auto s = static_cast<ElementCache>(static_cast<uint8_t>(tail[0]));
  return s == ElementCache::Complete || s == ElementCache::TooBig ? s : ElementCache::Unknown;
}

}  // namespace datadam
