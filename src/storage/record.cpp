#include "storage/record.h"

#include <cstring>

namespace datadam {

namespace {

template <class T>
void put_le(char* p, T v) {
  for (size_t i = 0; i < sizeof(T); ++i) p[i] = static_cast<char>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFF);
}

template <class T>
T get_le(const char* p) {
  uint64_t v = 0;
  for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return static_cast<T>(v);
}

}  // namespace

std::string_view value_type_name(ValueType t) {
  switch (t) {
    case ValueType::String: return "string";
    case ValueType::Hash: return "hash";
    case ValueType::Set: return "set";
    case ValueType::ZSet: return "zset";
    case ValueType::List: return "list";
    case ValueType::Json: return "ReJSON-RL";  // what Redis with RedisJSON answers to TYPE
  }
  return "unknown";
}

std::string encode_record(const RecordView& r) {
  std::string out(kRecordHeaderSize + r.value.size(), '\0');
  char* p = out.data();
  p[0] = static_cast<char>(kRecordFormat);
  p[1] = static_cast<char>(r.type);
  put_le<uint32_t>(p + 2, r.flags);
  put_le<int64_t>(p + 6, r.expire_at_ms);
  put_le<uint64_t>(p + 14, r.cas);
  if (!r.value.empty()) std::memcpy(p + kRecordHeaderSize, r.value.data(), r.value.size());
  return out;
}

bool decode_record(std::string_view raw, RecordView* out) {
  if (raw.size() < kRecordHeaderSize || static_cast<uint8_t>(raw[0]) != kRecordFormat) return false;
  const char* p = raw.data();
  out->type = static_cast<ValueType>(static_cast<uint8_t>(p[1]));
  out->flags = get_le<uint32_t>(p + 2);
  out->expire_at_ms = get_le<int64_t>(p + 6);
  out->cas = get_le<uint64_t>(p + 14);
  out->value = raw.substr(kRecordHeaderSize);
  return true;
}

std::string encode_collection_meta(const CollectionMeta& m) {
  std::string out(kCollectionMetaSize, '\0');
  put_le<uint64_t>(out.data(), m.version);
  put_le<uint64_t>(out.data() + 8, m.size);
  put_le<uint64_t>(out.data() + 16, m.head);
  return out;
}

bool decode_collection_meta(std::string_view payload, CollectionMeta* out) {
  if (payload.size() < kCollectionMetaSize) return false;
  out->version = get_le<uint64_t>(payload.data());
  out->size = get_le<uint64_t>(payload.data() + 8);
  out->head = get_le<uint64_t>(payload.data() + 16);
  return true;
}

}  // namespace datadam
