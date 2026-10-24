#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "storage/record.h"

namespace datadam {

class RocksStorage;

// The change feed (CDC): what each committed write batch did, decoded from
// the retained RocksDB WAL into logical changes.
//
//   op        key   args
//   set       k     value expire_at_ms          a string was written
//   json      k     document expire_at_ms       a JSON document was written (compact text)
//   del       k                                 the key was removed
//   meta      k     type size expire_at_ms version [head]   a collection's record
//                   was written (created, resized, TTL); a new version means
//                   the old contents are gone. head: lists only.
//   hset/hdel k     field [value]
//   sadd/srem k     member
//   zadd      k     score member
//   zrem      k     member
//   lset/ldel k     position [value]            position - head = index
//   flushall  ""                                every key was removed
//   data/datadel k  version sub-key [value]     an element of unknown type
//
// Expirations made by compaction are not in the WAL: `set` and `meta` carry
// expire_at_ms. Secondary index entries are derived data and left out.
struct Change {
  std::string op;
  std::string key;
  std::vector<std::string> args;
};

struct ChangeBatch {
  uint64_t seq = 0;      // sequence number of the batch's first write
  int64_t time_ms = 0;   // commit time (0: not recorded)
  std::vector<Change> changes;
};

struct ChangeRead {
  std::vector<ChangeBatch> batches;
  uint64_t next = 0;  // sequence to continue from
};

// Reads whole batches from sequence `from` (0: the oldest kept) until at
// least max_changes changes were collected, max_batches batches were looked
// at, or the WAL ends. keep(key) filters the changes ("flushall" always
// stays); batches left empty are dropped. Throws StorageError if the WAL is
// not kept or `from` is older than what is kept.
ChangeRead read_changes(const RocksStorage& storage, uint64_t from, size_t max_changes, size_t max_batches,
                        const std::function<bool(std::string_view key)>& keep);

// The note a write batch carries in the WAL (RocksStorage::Batch::put_log_data)
// so its changes can be decoded: the commit time, and the type of each
// collection version whose elements it writes (its data prefix, see data_keys.h).
std::string change_note(int64_t time_ms);
void add_change_note_type(std::string& note, std::string_view data_prefix, ValueType type);

}  // namespace datadam
