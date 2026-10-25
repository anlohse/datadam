// libFuzzer target: the stored encodings, which come back from disk and from
// a primary (replication): records, collection metadata and the element sets
// of small collections. Malformed input must be refused, never read past.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "db/element_set.h"
#include "storage/data_keys.h"
#include "storage/record.h"

using namespace datadam;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view raw(reinterpret_cast<const char*>(data), size);

  std::string_view key, sub;
  uint64_t version = 0;
  if (decode_data_key(raw, &key, &version, &sub) &&
      (key.data() + key.size() > raw.data() + raw.size() || sub.data() + sub.size() > raw.data() + raw.size())) {
    __builtin_trap();
  }

  RecordView rec;
  if (decode_record(raw, &rec)) {
    CollectionMeta meta;
    decode_collection_meta(rec.value, &meta);
  }

  ElementSet set;
  if (set.load(std::string(raw))) {
    size_t n = 0;
    set.for_range("", "", false, [&](std::string_view sub, std::string_view value) {
      if (!set.get(sub)) __builtin_trap();  // every listed element can be found
      (void)value;
      ++n;
      return true;
    });
    if (n != set.size()) __builtin_trap();
    set.put("fuzz", "x");
    set.erase(raw.substr(0, raw.size() / 2));
    // An encoding loads back with the same elements.
    ElementSet again;
    if (!again.load(set.encode()) || again.size() != set.size()) __builtin_trap();
  }
  return 0;
}
