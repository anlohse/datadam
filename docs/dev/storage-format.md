# On-disk storage format

This page describes exactly what datadam writes to RocksDB: how the database is opened, the four column
families, the byte layout of every key and value, how collection versions make deletes O(1), and how the
compaction filters reclaim what is left behind. Everything here is implemented in
[`src/storage/`](../../src/storage/), [`src/db/database.cpp`](../../src/db/database.cpp) and
[`src/index/`](../../src/index/).

## Contents

- [Overview](#overview)
- [RocksDB setup](#rocksdb-setup)
- [Byte order conventions](#byte-order-conventions)
- [Meta column family: records](#meta-column-family-records)
- [Data column family: collection elements](#data-column-family-collection-elements)
- [Collection versions and the version mark](#collection-versions-and-the-version-mark)
- [System column family](#system-column-family)
- [Index column family](#index-column-family)
- [Change notes in the WAL](#change-notes-in-the-wal)
- [What a committed write batch contains](#what-a-committed-write-batch-contains)
- [Removing data: DeleteRange and compaction filters](#removing-data-deleterange-and-compaction-filters)
- [Worked example: one hash with two fields](#worked-example-one-hash-with-two-fields)
- [Related pages](#related-pages)

## Overview

All persistent state lives in one RocksDB database (the `dir` setting) with four column families. The code
refers to them through `enum class Cf : uint8_t { Meta, Data, Index, System }` in
[`rocks_storage.h`](../../src/storage/rocks_storage.h).

| `Cf` | RocksDB name | Holds | Compaction filter |
|---|---|---|---|
| `Cf::Meta` | `meta` | One record per user key: type, memcache flags, TTL, cas and an inline value | `TtlCompactionFilter` |
| `Cf::Data` | `data` | One entry per element of a hash, set, sorted set or list | `DataGcFilter` (via `DataGcFilterFactory`) |
| `Cf::Index` | `index` | Secondary index entries (`IDX.*` and `FT.*`) | `IndexGcFilter` (via `IndexGcFilterFactory`) |
| `Cf::System` | `default` | Internal state: version mark, index definitions, replication state | none |

There is no separate key namespace inside a column family: a meta key is the user key bytes as they are, so
iterating the `meta` column family visits user keys in byte order (this is what `SCAN`, `KEYS` and the index
backfill rely on).

## RocksDB setup

`RocksStorage::open` in [`rocks_storage.cpp`](../../src/storage/rocks_storage.cpp) builds the options from a
`StorageOptions` (filled from `Config` in [`main.cpp`](../../src/main.cpp)) and opens the database with these
column family descriptors, in this order:

```cpp
{rocksdb::kDefaultColumnFamilyName, plain},  // handles_[0] -> Cf::System
{"meta", meta},                              // handles_[1] -> Cf::Meta
{"data", data},                              // handles_[2] -> Cf::Data
{"index", index},                            // handles_[3] -> Cf::Index
```

`RocksStorage::handle(Cf)` maps the enum to these handles. The numeric column family ids that RocksDB assigns
(and that appear inside encoded write batches) are not assumed anywhere: `RocksStorage::cf_ids()` returns them
as a `CfIds` array (`meta, data, index, system`) and `cf_of()` maps an id back to a `Cf`. Replication sends
these ids to the replica so it can decode the primary's batches (see
[replication-and-cdc-internals.md](replication-and-cdc-internals.md)).

### Database options

| Option | Value | Notes |
|---|---|---|
| `create_if_missing`, `create_missing_column_families` | `true` | A new directory gets all four column families. |
| `max_background_jobs` | `rocksdb-background-jobs` (default 4) | Flushes and compactions. |
| `bytes_per_sync` | 1 MiB | Incremental sync of SST files while they are written. |
| `keep_log_file_num` | 5 | RocksDB's info `LOG` files, not the WAL. |
| `manual_wal_flush` | `true` only with `experimental-write-path wal-buffered` | See [write paths](#write-options-fsync-policies-and-write-paths). |
| `WAL_ttl_seconds` | `wal-retention` in whole seconds | 0 (default) deletes WAL files as soon as recovery no longer needs them. |
| `WAL_size_limit_MB` | `wal-retention-size` rounded up to MiB, or 0 when `wal-retention` is 0 | RocksDB checks it rarely; `RocksStorage::trim_wal` enforces it every second. |

WAL retention exists for the change feed and replication; it is described in
[replication-and-cdc-internals.md](replication-and-cdc-internals.md#wal-retention).

### Column family options

`column_family_options()` builds one `ColumnFamilyOptions` (`plain`) that all four column families share;
`meta`, `data` and `index` are copies with a compaction filter added.

| Option | Value |
|---|---|
| `write_buffer_size` | `rocksdb-write-buffer` (default 64 MiB), per column family |
| `max_write_buffer_number` | 4 |
| `level_compaction_dynamic_level_bytes` | `true` |
| `compression` | `kLZ4Compression` |
| `bottommost_compression` | `kZSTD` |
| Table format | `BlockBasedTable`, `block_size` 16 KiB |
| `filter_policy` | `NewBloomFilterPolicy(10)` (10 bits per key) |
| `index_type` | `kTwoLevelIndexSearch` (partitioned index) |
| `partition_filters` | `true`, `metadata_block_size` 4096 |
| `cache_index_and_filter_blocks` | `true`, `cache_index_and_filter_blocks_with_high_priority = true` |
| `pin_l0_filter_and_index_blocks_in_cache` | `true` |
| `pin_top_level_index_and_filter` | `true` |
| `block_cache` | one LRU cache shared by all column families |

The block cache is created with `NewLRUCache` with capacity `rocksdb-block-cache` (default 64 MiB) and
`high_pri_pool_ratio = 0.5`. Index and filter partitions live in the high-priority pool, so a large scan that
churns data blocks cannot evict the filters that make lookups of missing keys cheap.

### Write options, fsync policies and write paths

`write_options()` derives the `rocksdb::WriteOptions` of every write from the current fsync policy (an atomic,
changeable with `CONFIG SET`) and the write path (fixed at startup):

- `sync = (fsync == FsyncPolicy::Always)`
- `disableWAL = (write_path == WritePath::NoWal)`

`FsyncPolicy` (`always`, `everysec`, `no`) and `WritePath` are defined in
[`config.h`](../../src/config/config.h). `RocksStorage::sync_wal()` makes buffered WAL data durable:

| `experimental-write-path` | `WritePath` | Per write | `sync_wal()` |
|---|---|---|---|
| `wal` (default, the only safe one) | `Wal` | written to the WAL file before `Write` returns | `DB::SyncWAL()` |
| `wal-buffered` | `WalBuffered` | kept in RocksDB's WAL buffer (`manual_wal_flush`) | `DB::FlushWAL(sync = fsync != no)` |
| `no-wal` | `NoWal` | `disableWAL`: only memtable flushes persist | nothing |
| `memory` | `Memory` | `put`, `del` and `write` return before touching RocksDB (`skip_writes()`) | nothing |

The maintenance thread in `main.cpp` calls `sync_wal()` every second when the policy is `everysec` or the path
is `wal-buffered`; the `RocksStorage` destructor calls it once more unless the policy is `always`.
`Config::validate` rejects `fsync always` with any path other than `wal`.

Runtime changes: `set_fsync_policy` (atomic store), `set_block_cache_size` (`Cache::SetCapacity`) and
`set_write_buffer_size` (`DB::SetOptions` on every column family) back `CONFIG SET`. `flush()` flushes all
memtables (`SAVE`, `BGSAVE`); `compact()` runs `CompactRange` with `BottommostLevelCompaction::kForce` on every
column family and is used by tests to run the compaction filters over everything.

## Byte order conventions

Two conventions coexist. Keys, and anything that must sort, are big endian; the record header and the cached
element set, which are only ever decoded, are little endian.

| Structure | Integers | Defined in |
|---|---|---|
| Record header (`encode_record`) | little endian | [`record.cpp`](../../src/storage/record.cpp) |
| `CollectionMeta` payload | little endian | [`record.cpp`](../../src/storage/record.cpp) |
| `ElementSet` encoding (cache only) | little endian | [`element_set.cpp`](../../src/db/element_set.cpp) |
| Data keys, list positions, sorted set scores | big endian | [`data_keys.h`](../../src/storage/data_keys.h) |
| Index entry keys and values, index definitions | big endian | [`index_def.cpp`](../../src/index/index_def.cpp) |
| System values (version mark, `index:next-id`, `repl:offset`) | big endian | see [System column family](#system-column-family) |
| WAL change notes | big endian | [`change_feed.cpp`](../../src/db/change_feed.cpp) |

## Meta column family: records

Every user key has exactly one record in `meta`, keyed by the user key itself. The record is encoded by
`encode_record(const RecordView&)` and decoded by `decode_record()`:

```text
offset  size  field
     0     1  format         kRecordFormat = 1 (decode_record rejects anything else)
     1     1  type           ValueType
     2     4  flags          u32 LE, memcache client flags (0 for Redis writes)
     6     8  expire_at_ms   i64 LE, absolute Unix time in ms; 0 = no TTL
    14     8  cas            u64 LE, changes on every write
    22     n  payload        depends on type
```

`kRecordHeaderSize` is 22. `RecordView::expired(now_ms)` is `expire_at_ms != 0 && now_ms >= expire_at_ms`.

### Value types

```cpp
enum class ValueType : uint8_t { String = 0, Hash = 1, Set = 2, ZSet = 3, List = 4, Json = 5 };
```

| Type | Payload | `TYPE` reply (`value_type_name`) |
|---|---|---|
| `String` (0) | the value bytes | `string` |
| `Hash` (1) | `CollectionMeta` (24 bytes) | `hash` |
| `Set` (2) | `CollectionMeta` | `set` |
| `ZSet` (3) | `CollectionMeta` | `zset` |
| `List` (4) | `CollectionMeta` | `list` |
| `Json` (5) | the document as compact JSON text | `ReJSON-RL` |

`is_collection()` is true for types 1 to 4. A JSON document is a single record like a string: every
`JSON.*` write parses the text, changes it and writes it back whole (see
[`cmd_json.cpp`](../../src/server/cmd_json.cpp)).

### flags and cas

- `flags` is the 32-bit client flags value of the memcache `set` family (rejected above `UINT32_MAX` by
  [`memcache_protocol.cpp`](../../src/server/memcache_protocol.cpp)) and is returned by `get`/`gets`.
- `cas` is assigned by `Database::store_locked` from `Database::next_cas()`, an in-memory counter that starts at
  `unix_time_ms() * 1000` when the process starts, so values keep increasing across restarts. Memcache `gets`
  returns it and `cas` compares against it. Writes that only change the TTL (memcache `touch`, `EXPIRE` and
  friends) use `Database::Op::PutKeepCas` and keep it. Collections get a new cas whenever their meta record is
  rewritten.

### CollectionMeta

The payload of a hash, set, sorted set or list is a `CollectionMeta` (`kCollectionMetaSize` = 24):

```text
offset  size  field
     0     8  version   u64 LE  collection version; every data key of the collection carries it
     8     8  size      u64 LE  number of elements
    16     8  head      u64 LE  lists: position of the first element; others: kListStart
```

`CollectionMeta::kListStart` is `1 << 63`, so a list can grow in both directions without its positions
wrapping. A new collection (`KeyTxn::reset`) starts with `head = kListStart` whatever its type.
`decode_collection_meta()` reads only the first 24 bytes, because a cached value may carry more (next section).
An empty collection does not exist: `KeyTxn::finish` deletes the record when `size` drops to 0.

### The element cache tail (never on disk)

In the object cache only, the value of a collection can be followed by the elements themselves:

```text
cached value = CollectionMeta (24 bytes) | u8 ElementCache | (Complete: ElementSet encoding)

enum class ElementCache : uint8_t { Unknown = 0, Complete = 1, TooBig = 2 };

ElementSet encoding (all u32 little endian):
  u32 count | count x ( u32 sub-key length | sub-key | u32 value length | value )   sorted by sub-key
```

This tail is not persisted. `Database::store_locked` cuts a collection's value back to 24 bytes before
`encode_record`, and `rename` rewrites the payload with `encode_collection_meta`. RocksDB therefore always holds
exactly 22 + 24 = 46 bytes for a collection record. The cache side is described in
[memory-and-cache.md](memory-and-cache.md).

## Data column family: collection elements

Each element of a collection is one entry in `data`. The key layout is defined in
[`data_keys.h`](../../src/storage/data_keys.h):

```text
+----------------+-------------+--------------------+-------------------+
| u32 key length |  user key   |  u64 version       |  sub-key          |
|  (big endian)  | (len bytes) |  (big endian)      |  (type specific)  |
+----------------+-------------+--------------------+-------------------+
 \______________________ data_prefix(key, version) _/
```

- `data_prefix(key, version)` builds the first `4 + key.size() + 8` bytes. All elements of one collection
  version are contiguous and sorted by sub-key, and the prefix of version `v + 1` is the exclusive upper
  bound of version `v`.
- `decode_data_key()` splits a data key back into user key, version and sub-key (false if shorter than 12
  bytes or the length does not fit).
- The length prefix keeps keys of different users apart: without it, the elements of `ab` would interleave
  with those of `a` followed by a sub-key starting with `b`.

### Sub-keys and values per type

| Type | Sub-key | Value | Written by |
|---|---|---|---|
| Hash | field name | field value | [`cmd_hash.cpp`](../../src/server/cmd_hash.cpp) |
| Set | member | empty | [`cmd_set.cpp`](../../src/server/cmd_set.cpp) |
| Sorted set | `'m'` + member | `encode_u64(sortable_score(score))` (8 bytes) | [`cmd_zset.cpp`](../../src/server/cmd_zset.cpp) |
| Sorted set | `'s'` + `be64(sortable_score(score))` + member | empty | `cmd_zset.cpp` |
| List | `encode_u64(meta.head + i)` (8 bytes, big endian) | element | [`cmd_list.cpp`](../../src/server/cmd_list.cpp) |

A sorted set member therefore has two entries: the `'m'` entry answers "what is the score of this member" and
gives lexicographic order (`ZRANGEBYLEX`), the `'s'` entry gives score order. A list element `i` (0-based) lives
at position `head + i`; pushing on the left decrements `head`, pushing on the right appends at `head + size`,
and inserting or removing in the middle rewrites the tail.

### sortable_score

`sortable_score(double)` maps a double to a `u64` whose unsigned order matches numeric order:

```cpp
if (d == 0) d = 0;                                     // -0.0 becomes +0.0
uint64_t bits = std::bit_cast<uint64_t>(d);
return (bits & (1ULL << 63)) ? ~bits                   // negative: flip every bit
                             : bits | (1ULL << 63);    // positive: set the sign bit
```

`-inf < ... < -0 == 0 < ... < +inf`. `score_from_sortable()` inverts it. The same function encodes NUMERIC
index values (see [indexes-internals.md](indexes-internals.md#value-encodings)). Range bounds use
`sortable_score(x) + 1` as "just past `x`".

## Collection versions and the version mark

Every collection version is unique for the lifetime of the data directory: a collection created, replaced or
re-created gets a version that has never been used, not even before a restart. Because every data key carries
the version, deleting or overwriting a collection only has to change (or remove) its meta record; the old
elements become invisible at once and are reclaimed later (see
[Removing data](#removing-data-deleterange-and-compaction-filters)).

Versions come from `Database::next_version()`:

```cpp
uint64_t v = version_.fetch_add(1) + 1;
if (v > version_limit_) {                          // under version_mu_
  uint64_t mark = std::max(limit, v) + kVersionBlock;   // kVersionBlock = 1 << 20
  storage_.put(Cf::System, "collection-version", encode_u64(mark));
  version_limit_ = mark;
}
```

- The mark is written with a direct `RocksStorage::put` (its own RocksDB write, outside any pending batch), so
  it reaches the WAL before any data entry that uses a version of the new block.
- At startup the `Database` constructor reads the mark and sets both `version_` and `version_limit_` to it, so
  the first version after a restart is `mark + 1`. Up to `kVersionBlock` versions are skipped per restart,
  which costs nothing.
- Version 0 is never allocated, which lets the compaction filter use 0 for "no live collection".
- A replica receives the primary's mark through replication; `Database::promote()` reads it again so a
  promoted replica continues past every version the old primary may have used.

## System column family

The default column family holds a handful of internal keys. This is the complete list of `Cf::System` users:

| Key | Value | Written by | Read by |
|---|---|---|---|
| `collection-version` | `be64` version mark | `Database::next_version` | `Database::Database`, `Database::promote` |
| `index:next-id` | `u32` big endian: the next index id | `IndexRegistry::create` | `IndexRegistry::load` |
| `index:def:<name>` | `IndexDef::encode()` | `IndexRegistry::create`, `IndexRegistry::set_state` (via `persist_locked`) | `IndexRegistry::load` |
| `repl:id` | 40 lowercase hex characters: this server's replication id | `ReplicationState::own_id` / `new_own_id`, `finish_full_sync` | `ReplicationState::own_id` |
| `repl:primary-id` | replica: the replication id of the primary it follows | `finish_full_sync` | `connect_and_sync` |
| `repl:offset` | replica: `be64` next primary sequence to fetch | `finish_full_sync`, every applied `wal` message | `connect_and_sync` |

- `IndexRegistry::drop` deletes `index:def:<name>` in the same batch as the `DeleteRange` over the index's
  entries; `index:next-id` is never decreased, so index ids are never reused. Index definitions are described in
  [indexes-internals.md](indexes-internals.md#definition-encoding).
- The `repl:` keys are this server's own state: the primary skips them when it sends a full-sync snapshot and
  the replica skips them in every batch it applies (`ReplicaBatch::skip`). Everything else in `System` is
  replicated. `Server::follow` deletes `repl:primary-id` and `repl:offset` on promotion.
- `FLUSHALL` (`RocksStorage::delete_all`) clears only the other three column families, so index definitions and
  the version mark survive it. `Database::clear_for_full_sync` deletes the `System` keys as well.

## Index column family

Index entries are keyed by a big-endian `u32` index id followed by an order-preserving tuple and, except in
UNIQUE indexes, the owner key; the value carries the owner's collection version and TTL:

```text
u32 index id | tuple | owner key       ->  be64 version | be64 expire_at_ms
u32 index id | tuple          (UNIQUE) ->  be64 version | be64 expire_at_ms | owner key
```

The encodings, the tuple sets and the maintenance rules are in
[indexes-internals.md](indexes-internals.md#entry-layout).

## Change notes in the WAL

When the WAL is retained (`RocksStorage::wal_retained()`: `wal-retention > 0` with the `wal` or `wal-buffered`
write path), the `Database` sets `change_notes_` at construction and every user-data write batch carries a
blob written with `WriteBatch::PutLogData` (`RocksStorage::Batch::put_log_data`). Log data goes to the WAL with
the batch but is never stored in any column family, does not consume a sequence number and does not count in
`WriteBatch::Count()`.

```text
'C' (0x43) | u8 format = 1 | be64 commit time (Unix ms)
  then, per collection version whose elements the batch writes:
    be32 data prefix length | data prefix (u32 key length | key | be64 version) | u8 ValueType
```

`change_note()` builds the header and `add_change_note_type()` appends one type entry, both in
[`change_feed.cpp`](../../src/db/change_feed.cpp). The types are needed because a data key alone does not say
whether `sub-key -> value` is a hash field, a set member, a sorted set entry or a list position. How notes are
staged and decoded is covered in
[replication-and-cdc-internals.md](replication-and-cdc-internals.md#change-notes).

## What a committed write batch contains

Most writes go through a `Database::PendingBatch` (one per pipeline, see [concurrency.md](concurrency.md)). It
keeps only the last write per key in four containers and `Database::commit_batch` turns them into a single
`rocksdb::WriteBatch` in this order:

1. the change note, if `change_notes_` (with one type entry per `element_types` item);
2. meta puts and deletes (`writes`, an `unordered_map`, so in no particular order);
3. data puts and deletes (`data`, a `std::map`, in key order);
4. index puts and deletes (`index`, a `std::map`, in key order);
5. `DeleteRange` on `data` for every `range_deletes` item (dead collection versions only).

A pipeline that writes the same key many times therefore produces one put. Writes made outside a batch
(`mset`, `rename`, `flush_all`, `IndexRegistry` changes, the version mark, replication state) build their own
`RocksStorage::Batch` or call `RocksStorage::put` directly; `mset`, `rename`, `flush_all` and the single
`write_storage`/`delete_storage` writes also add a change note when `change_notes_` is on.

## Removing data: DeleteRange and compaction filters

Old elements and index entries are never looked up again once their collection version is dead, so they can be
removed lazily. Three mechanisms do it.

### DeleteRange

| Caller | Column family | Range |
|---|---|---|
| `Database::drop_elements`, `KeyTxn::drop_elements` | `data` | `[data_prefix(key, v), data_prefix(key, v + 1))` |
| `Database::update` (collection deleted or replaced) | `data` | same, via `drop_elements` |
| `Database::load_locked` (lazy expiration of a collection) | `data` | same |
| `Database::rename` | `data` | the source version, and the old destination version |
| `IndexRegistry::drop` | `index` | `[encode_index_id(id), prefix_end(encode_index_id(id)))` |
| `RocksStorage::delete_all` (`FLUSHALL`, full sync) | `meta`, `data`, `index` | first key to last key, plus a `Delete` of the last key (the end is exclusive) |

Blind overwrites do not delete ranges: `Database::put` (`SET` over a hash) and `Database::mset` write the new
record without reading the elements of the collection they replace. Those elements are left to the data
compaction filter.

### TtlCompactionFilter (meta)

A single static instance attached as `meta.compaction_filter`. `Filter()` decodes the record and removes it when
`RecordView::expired(unix_time_ms())`; a value that does not decode is kept. Keys that expire and are never read
again thus free their space, with no write through the WAL (which is why the change feed cannot report such
expirations).

### DataGcFilter (data)

Created per compaction by `DataGcFilterFactory`. For each data key it decodes the user key and version, and
looks up the user key's meta record with `DB::Get` on the live database (`gc_db_`). The result is cached for the
last user key seen, since a compaction visits all elements of a collection together.

- The live version is the record's `CollectionMeta::version` if the record exists, decodes, is a collection type
  and is not expired; otherwise it is 0.
- The entry is dropped when `version != live_version`, so elements of deleted, expired, overwritten or retyped
  collections go away.
- When in doubt it keeps the entry: undecodable key, a `Get` error, or `gc_db_` still null (the database is
  opening or closing).

### IndexGcFilter (index)

Created per compaction by `IndexGcFilterFactory`. It asks the `IndexOwnerResolver` (the `IndexRegistry`, set
with `RocksStorage::set_index_resolver` by the `Database` constructor) who owns the entry:

- `owner_of` returns -1 when the index id is unknown (the index was dropped): the entry is dropped. During a
  replica's full sync the GC is paused (`IndexRegistry::pause_gc`) and `owner_of` keeps everything, because
  the entries arrive before the definitions.
- It returns 0 when the key is shorter than 4 bytes or the entry does not split: the entry is kept.
- Otherwise it returns the owner key, whether the index is over JSON documents, and the version stored in the
  entry value. The filter reads the owner's meta record and drops the entry if the record is missing, does not
  decode or is expired; for a JSON index if the record is not `Json`; for a hash index if the record is not a
  `Hash` or its version differs from the entry's.

The filter does not check that the tuple still matches the fields; within one version, entries are kept exact by
the writes themselves (see [indexes-internals.md](indexes-internals.md#garbage-collection)).

## Worked example: one hash with two fields

Start from an empty data directory with `wal-retention 1h` (so change notes are on) and no indexes, and run:

```text
HSET user:1 city SP age 30
```

`hset_generic` calls `KeyTxn::create(Hash)`, which calls `KeyTxn::reset`, which calls `next_version()`. That is
the first version ever: `v = 1 > version_limit_ = 0`, so the mark becomes `1 + 2^20 = 1048577` and is written
immediately, as its own RocksDB write (sequence 1):

```text
System  put  "collection-version"  ->  00 00 00 00 00 10 00 01        (be64 1048577)
```

The command then stages two data puts and, in `KeyTxn::finish`, the meta record (size 2, version 1, a new cas).
When the pipeline's batch commits, this `WriteBatch` is written (sequence 2, `Count()` = 3):

```text
LogData  43 01                                   'C', format 1
         <be64 commit time in ms>
         00 00 00 12                             data prefix length = 18
         00 00 00 06 75 73 65 72 3a 31           u32 6, "user:1"
         00 00 00 00 00 00 00 01                 version 1
         01                                      ValueType::Hash

Meta  put  "user:1"  (75 73 65 72 3a 31)
      ->  01                                     kRecordFormat
          01                                     ValueType::Hash
          00 00 00 00                            flags = 0
          00 00 00 00 00 00 00 00                expire_at_ms = 0 (no TTL)
          <u64 LE cas>                           next_cas()
          01 00 00 00 00 00 00 00                CollectionMeta.version = 1
          02 00 00 00 00 00 00 00                CollectionMeta.size = 2
          00 00 00 00 00 00 00 80                CollectionMeta.head = 1 << 63 (kListStart)
                                                 46 bytes in total

Data  put  00 00 00 06 75 73 65 72 3a 31 00 00 00 00 00 00 00 01 61 67 65        "user:1" v1 "age"
      ->  33 30                                                                   "30"
Data  put  00 00 00 06 75 73 65 72 3a 31 00 00 00 00 00 00 00 01 63 69 74 79     "user:1" v1 "city"
      ->  53 50                                                                   "SP"
```

The data puts appear in key order (`age` before `city`) because `PendingBatch::data` is a `std::map`, not in the
order of the command's arguments. The object cache receives the meta record plus an `ElementCache::Complete` tail
holding both fields; none of that tail reaches RocksDB. `CDC.READ` decodes this batch as
`meta user:1 hash 2 0 1`, `hset user:1 age 30`, `hset user:1 city SP`.

What happens next:

- `HDEL user:1 age` writes a `Delete` of the `age` data key and rewrites the meta record with size 1.
- `DEL user:1` writes a `Delete` of the meta key and a `DeleteRange` from
  `00 00 00 06 "user:1" 00..01` to `00 00 00 06 "user:1" 00..02`.
- `SET user:1 x` (a blind write) only replaces the meta record with a `String` record. The two data entries stay
  until a compaction of `data` meets them: `DataGcFilter` finds that `user:1` is no longer a collection
  (live version 0) and drops them.
- The second hash created later gets version 2, and no `collection-version` write happens until version
  1048578 is needed.

## Related pages

- [indexes-internals.md](indexes-internals.md): index entries, definitions and their maintenance.
- [replication-and-cdc-internals.md](replication-and-cdc-internals.md): WAL retention, change notes, replication.
- [memory-and-cache.md](memory-and-cache.md): the object cache and the cached element sets.
- [concurrency.md](concurrency.md): write batches, key locks and transactions.
- [architecture.md](architecture.md): how storage, cache and server fit together.
