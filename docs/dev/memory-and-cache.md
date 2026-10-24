# Memory pool and object cache

The object cache keeps recently used records in front of RocksDB, in a memory pool of its own: a reserved address
range split into pages (`PageArena`), carved into size classes by per-shard slab allocators (`SlabAllocator`),
and indexed by a sharded hash table with per-class LRU lists (`ObjectCache`). This page describes each layer,
the eviction and admission policies, how small collections are cached whole, and what `CACHE STATS` and
`CACHE CLASSES` report.

## Contents

- [Overview](#overview)
- [PageArena](#pagearena)
- [Size classes](#size-classes)
- [SlabAllocator](#slaballocator)
- [ObjectCache](#objectcache)
- [Item layout](#item-layout)
- [Eviction](#eviction)
- [Admission filter](#admission-filter)
- [Objects that are not cached](#objects-that-are-not-cached)
- [Small collections cached whole](#small-collections-cached-whole)
- [CACHE STATS and CACHE CLASSES](#cache-stats-and-cache-classes)
- [Tests](#tests)

## Overview

```text
ObjectCache
 ├── Doorkeeper                      admission filter (cache/admission.h)
 ├── SizeClasses                     chunk sizes, 88 B .. 72 B + cache-max-object-size
 ├── PageArena (shared)              cache-max-memory of reserved address space, in pages
 └── Shard x cache-shards (default 16), each with its own mutex:
       ├── buckets                   intrusive hash table (vector<Item*>, chained through Item::hnext)
       ├── SlabAllocator             pages taken from the arena, one size class per page
       ├── lru[class]                one LRU list per size class
       └── stats, crawl_cursor
```

The options come from `CacheOptions` ([object_cache.h](../../src/cache/object_cache.h)), filled in `main()` from
the configuration:

| Option | `CacheOptions` field | Default | Changeable at run time |
|---|---|---|---|
| `cache-max-memory` | `max_memory` | 512 MiB | no |
| `cache-min-memory` | `min_memory` | 64 MiB | idle floor only (see below) |
| `cache-max-object-size` | `max_object_size` | 4096 (64 B to 1 MiB) | no |
| `cache-max-idle` | `max_idle_ms` | 5 min (0: off) | yes |
| `cache-max-age` | `max_age_ms` | 0 (off) | yes |
| `cache-shards` | `shards` | 16 (clamped to 1..1024) | no |
| `cache-admission` | `admission` | yes | yes |
| `cache-maintenance-interval` | (the `Maintenance` thread) | 1000 ms | no |
| `cache-on-write` | (`Database::cache_on_write_`) | yes | yes |

`CONFIG SET` applies the run-time ones through `ObjectCache::set_eviction` and `set_admission`
([server_context.cpp](../../src/server/server_context.cpp)).

## PageArena

[page_arena.h](../../src/mem/page_arena.h), [page_arena.cpp](../../src/mem/page_arena.cpp).

- **Reservation.** The constructor reserves `max_pages * page_size` bytes of contiguous address space, where
  `max_pages = cache-max-memory / page_size`: `VirtualAlloc(MEM_RESERVE, PAGE_NOACCESS)` on Windows,
  `mmap(PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)` elsewhere. Nothing is backed by
  memory yet. With `max_pages == 0` (a `cache-max-memory` smaller than one page) nothing is reserved and the
  cache is disabled (`ObjectCache::enabled()` is false; `main()` logs a warning).
- **Page size.** `ObjectCache::compute_page_size` picks `max(64 KiB, bit_ceil(largest chunk) * 4)`: small pages
  keep a page steal cheap, and a page always holds at least four of the largest chunks. With the default
  4096-byte objects the largest chunk is 4168 bytes, so pages are 64 KiB; with `cache-max-object-size 1mb` they
  are 8 MiB.
- **Commit.** `acquire()` returns a page index, preferring a free page that is still committed, then a free
  decommitted one, then a never-used one; the latter two are committed (`VirtualAlloc(MEM_COMMIT)` on Windows; a
  no-op elsewhere, where the kernel backs pages when they are touched). It returns `kNone` when every page is in
  use.
- **Decommit and the minimum.** `release()` keeps the page committed for reuse while no more than
  `min_pages = cache-min-memory / page_size` pages are committed; beyond that it decommits it
  (`VirtualFree(MEM_DECOMMIT)` or `madvise(MADV_DONTNEED)`), returning the memory to the operating system.
- **Metadata.** Outside the pool the arena keeps one `PageMeta` per page (owner shard, size class, chunk size,
  capacity, bump pointer, free-list head, live count, partial-list links) and a bitmap of live chunks per page.
  Only the slab allocator that owns a page touches its metadata, under its shard's lock. The arena's own free
  lists are under `PageArena::mu_`.

`cache-min-memory` thus has two roles: the number of committed pages the arena keeps (fixed at startup), and the
floor of idle eviction (`ObjectCache::min_memory_`, which `CONFIG SET` can change).

## Size classes

[size_classes.h](../../src/mem/size_classes.h): `SizeClasses(min_chunk, max_chunk, factor = 1.25)` builds
geometric chunk sizes like memcached's slab classes. Each size is 8-byte aligned and at least 8 bytes larger than
the previous one; the last class is exactly `max_chunk`. If that gives more than `kMaxClasses` (64) classes, the
factor is multiplied by 1.1 until it fits. `class_for(bytes)` is a binary search for the smallest class that
holds `bytes`.

The cache builds its classes from `sizeof(Item) + 16` (88 bytes) to `sizeof(Item) + cache-max-object-size`, so
an object of key + value bytes goes to the class of `72 + key + value`, and the internal waste is bounded by the
growth factor.

## SlabAllocator

[slab_allocator.h](../../src/mem/slab_allocator.h), [slab_allocator.cpp](../../src/mem/slab_allocator.cpp). One
per shard, not thread-safe (the shard mutex serializes it).

- A page taken from the arena is dedicated to one class and split into `page_size / chunk_size` chunks.
- `partial_[class]` is a doubly linked list (through `PageMeta::prev/next`) of the pages of that class that have
  a free chunk.
- **allocate(class)** takes the first partial page (or a new page from the arena; `nullptr` if there is none),
  then its free-list head (freed chunks store the index of the next free one in their first 4 bytes) or the next
  never-used chunk (`bump`). It sets the chunk's live bit and unlinks the page when it becomes full. O(1).
- **deallocate(chunk)** clears the live bit; a page that becomes empty goes back to the arena, so any class or
  shard can reuse it; otherwise the chunk is pushed on the page's free list and a page that was full is linked
  into the partial list again.
- **live_chunks(page)** walks the live bitmap; the cache uses it to empty a page during a page steal.

## ObjectCache

[object_cache.h](../../src/cache/object_cache.h), [object_cache.cpp](../../src/cache/object_cache.cpp).

- **Sharding.** A key's 64-bit `hash_bytes` picks the shard with its high half (`(h >> 32) % shards`) and the
  bucket with its low 32 bits, which `Item::hash` stores. Each shard has its own mutex, so threads touching
  different shards never contend.
- **Hash table.** Power-of-two bucket vector (1024 initially), chained through `Item::hnext`; it doubles
  (`grow`) when the shard holds more items than buckets. The items themselves live in slab chunks, so the table
  is intrusive: no separate node allocation.
- **LRU.** One doubly linked list per size class per shard, through `Item::lru_prev/lru_next`. A hit moves the
  item to the front and updates `last_access`.
- **lookup(key, fn, count_stats)** finds the item, evicts it if it is past `cache-max-age` (`evicted_age`) or its
  key's TTL (`evicted_ttl`), and otherwise calls `fn(RecordView)` under the shard mutex, with the value pointing
  into the chunk (`GET` replies straight from pooled memory). Internal lookups pass `count_stats = false` so they
  do not distort `hits` / `misses`.
- **insert(key, rec, admit)** removes any existing item for the key, rejects objects that are not cacheable
  (`rejected_too_large`) or already expired, applies the admission filter (see below), allocates a chunk of the
  right class (evicting if needed), and copies key and value into it. It returns whether the object is cached.
- **erase**, **clear**: remove one key, or every item of every shard (`CACHE CLEAR`, `FLUSHALL`, a full sync).
- **defer_miss(key)** un-counts a miss for a read that the read pool will retry (see
  [concurrency.md](concurrency.md#the-read-pool)).

## Item layout

Every cached object is one chunk: a 72-byte header (`static_assert(sizeof(Item) == 72)`), then the key, then the
value.

| Field | Type | Bytes | Meaning |
|---|---|---|---|
| `lru_prev`, `lru_next` | `Item*` | 16 | Per-class LRU links |
| `hnext` | `Item*` | 8 | Hash chain |
| `expire_at_ms` | `int64_t` | 8 | Key TTL, Unix ms (0: none) |
| `cas` | `uint64_t` | 8 | Record version |
| `loaded_at` | `int64_t` | 8 | Steady ms when inserted (for `cache-max-age`) |
| `last_access` | `int64_t` | 8 | Steady ms of the last hit (LRU, `cache-max-idle`) |
| `hash` | `uint32_t` | 4 | Low half of the key hash |
| `flags` | `uint32_t` | 4 | memcache client flags |
| `value_len` | `uint32_t` | 4 | Value length |
| `key_len` | `uint16_t` | 2 | Key length (keys over 65535 bytes are never cached) |
| `type` | `uint8_t` | 1 | `ValueType`, plus `kPendingBit` (0x80): a write not committed yet, which lock-free lookups treat as a miss (see [concurrency.md](concurrency.md#publishing-to-the-cache)) |
| `cls` | `uint8_t` | 1 | Size class |

`Item::view()` turns it into a `RecordView` ([record.h](../../src/storage/record.h)) without copying.

## Eviction

| Trigger | Where | What goes |
|---|---|---|
| No free chunk and no free page | `ObjectCache::allocate` | Least recently used memory of the shard, up to 16 attempts (`kMaxAllocAttempts`); see below |
| `cache-max-idle` | `run_maintenance` | Items not read for that long, oldest first, at most 10,000 per shard per pass, while the shard's slab holds more than `cache-min-memory / shards` bytes (`evicted_idle`) |
| `cache-max-age` | `lookup`, `run_maintenance` | Items inserted longer ago than that, even hot ones (`evicted_age`) |
| Key TTL | `lookup`, `run_maintenance` | Items whose `expire_at_ms` passed (`evicted_ttl`) |

**Under memory pressure** `allocate` finds the shard's oldest item across all classes (`oldest()`: the LRU tail
with the smallest `last_access`). If the tail of the class being allocated is that item, or has been idle at least
half as long, it evicts that tail: a chunk of exactly the right size becomes free. Otherwise the oldest data is in
another class: it evicts every live item of the victim's page (`live_chunks`), the page returns to the arena and
the next attempt re-carves it for the needed class (`page_steals`, and each item counts in `evicted_lru`). When the
shard has nothing to evict (other shards hold every page) the allocation fails (`alloc_failures`) and the object
is simply not cached.

**Maintenance** runs from the `Maintenance` thread in [main.cpp](../../src/main.cpp) every
`cache-maintenance-interval`. Per shard, under its mutex, it first evicts idle items as above, then crawls
`kCrawlBucketsPerPass` (4096) buckets from `crawl_cursor`, removing items past `cache-max-age` or their TTL. Over
successive passes the crawl covers the whole table.

## Admission filter

[admission.h](../../src/cache/admission.h): `Doorkeeper`, the doorkeeper of TinyLFU. With a full pool, a plain LRU
lets every one-off read or write push out an item, so a bulk load or a scan replaces the whole cache.

- **When it applies.** In `insert`, only for a key that is not cached already, when the caller did not pass
  `admit = true`, `cache-admission` is on, and the pool is (nearly) full: `pages_in_use + shards >= max_pages`.
  Then the key is admitted only if `seen_or_add(hash)` finds it; otherwise it is remembered and the insert is
  refused (`rejected_admission`). A key therefore gets in on its second access within the window.
- **Structure.** Two generations of a bloom filter with two bits per key (the hash and a rotated, multiplied copy
  of it). The window is `max(65536, cache-max-memory / 512)` keys (about as many keys as the cache holds, at 512
  bytes per item), with about 8 bits per key (rounded up to a power of two), which keeps false positives near 2%.
  A lookup tests both generations; an addition sets bits in the current one. After `window` additions the older
  generation is cleared and becomes current, so "recently" means within the last one to two windows.
- **Bypass.** Replacing a cached key is always allowed. The read pool inserts with `admit = true`
  (`Database::load_into_cache`): a client is waiting for that key.

## Objects that are not cached

`ObjectCache::cacheable(key_len, value_len)` requires an enabled cache, `key_len <= 65535` and
`key_len + value_len <= cache-max-object-size`. An object that fails it is never stored: `insert` removes any
older cached copy and counts `rejected_too_large`. Such keys are always read from RocksDB, whose block cache
(`rocksdb-block-cache`) is their only cache. For deferred reads, `load_into_cache` returns false for them and the
command runs again reading the disk on the I/O thread.

Writes populate the cache (`Database::cache_store`) only with `cache-on-write yes`; with `no` a write erases the
cached copy and the next read loads it.

## Small collections cached whole

A collection's record in RocksDB holds only its 24-byte `CollectionMeta`; the elements are in the `data` column
family (see [storage-format.md](storage-format.md)). In the cache, the value of a collection record may carry a
tail after the meta ([record.h](../../src/storage/record.h)):

```text
cached value:  CollectionMeta (24 bytes) | u8 ElementCache | (Complete: ElementSet encoding)
ElementCache:  Unknown = 0 (no tail, or unknown) | Complete = 1 | TooBig = 2
```

- **What qualifies.** A collection of at most `Database::kMaxCachedElements` (128) elements whose whole record
  (meta, marker and encoding) passes `cacheable`. `Database::element_tail` produces the `Complete` tail, or a
  `TooBig` marker otherwise.
- **Loading.** When a transaction opens a collection that is not cached whole, has at most 128 elements and is
  not known to be too big, the `KeyTxn` constructor scans all its elements once into an `ElementSet`; the
  elements are published to the cache when the batch commits (`publish_elements`, see
  [concurrency.md](concurrency.md#publishing-to-the-cache)).
- **Hysteresis.** When a transaction writes the meta of a collection whose elements are not in memory,
  `KeyTxn::partial_cache_tail` keeps the `TooBig` marker only while the collection has more than 64 elements
  (`kMaxCachedElements / 2`); below that the marker becomes `Unknown`, and the next command tries to load it whole
  again.
- **Reads.** `Database::peek` (used by `read_collection`) serves a read from the cached record without the key
  lock; with `need_elements` it requires the `Complete` marker. Commands that only need the meta (`HLEN`, `SCARD`,
  `TYPE`, `TTL`, ...) are served even for large collections, since their meta is cached.
- **What RocksDB sees.** `store_locked` strips the tail before encoding the record for RocksDB; only the cache
  ever holds elements in the record.

**ElementSet** ([element_set.h](../../src/db/element_set.h)) is the in-memory form. Its encoding is a little-endian
`u32` count followed, per element in sub-key order, by `u32` length + sub-key + `u32` length + value. `load` takes
the cached buffer itself and makes one pass to record each entry's offset, with no allocation per element;
lookups (`get`, `lower_bound`) are binary searches over those offsets. Changes go to an ordered `changes_` map (a
value, or `nullopt` for a removal) that `for_range` merges with the base, a change hiding the base entry of the same
sub-key. `encode` returns the base unchanged when there are no changes, and re-encodes otherwise; `append` builds
a set in order while keeping the count prefix valid.

## CACHE STATS and CACHE CLASSES

`CACHE STATS` ([redis_commands.cpp](../../src/server/redis_commands.cpp), `cmd_cache`) replies a map built from
`ObjectCache::options()` and `ObjectCache::stats()` (which sums the per-shard counters under each shard mutex).
`INFO cache` (or `INFO memory`) reports the same values with a `cache_` prefix, plus `cache_hit_ratio`.

| Field | Meaning |
|---|---|
| `max_memory`, `min_memory`, `max_object_size`, `max_idle_ms`, `max_age_ms` | Options in effect (the eviction ones as last set) |
| `page_size` | Arena page size in bytes |
| `pages_in_use` | Pages owned by a slab allocator |
| `pages_committed` | Pages committed: in use plus free ones kept for reuse (on Linux, counted when handed out, even if never touched) |
| `used_memory` | `pages_in_use * page_size` |
| `chunk_bytes` | Bytes of chunks in use: header, key, value and the slack of the size class |
| `payload_bytes` | Key + value bytes of the cached items |
| `items` | Cached items |
| `hits`, `misses` | Counted lookups (`Database::read`, `get`, `exists`, `peek`); internal lookups are not counted, and a deferred read's first miss is un-counted |
| `inserts` | Successful inserts |
| `rejected_too_large` | Objects that were not cacheable |
| `rejected_admission` | New keys turned away by the admission filter |
| `alloc_failures` | Inserts that found no memory even after eviction |
| `evicted_lru` | Items evicted under memory pressure (including those of stolen pages) |
| `evicted_idle`, `evicted_age`, `evicted_ttl` | Items evicted by `cache-max-idle`, `cache-max-age`, or their key's TTL |
| `page_steals` | Pages emptied to move memory between size classes |

`CACHE CLASSES` replies one map per size class with `chunk_size`, `pages` (pages of that class across shards) and
`items` (items in that class's LRU lists across shards). It shows where the memory went: a class with many pages
and few items holds large chunks, and classes nobody uses have zero pages. `CACHE CLEAR` empties the cache without
touching data.

## Tests

- [object_cache_test.cpp](../../tests/object_cache_test.cpp): insert, large objects, LRU under pressure, page
  steal, idle eviction floor, max age, TTL, disabled cache, admission.
- [slab_allocator_test.cpp](../../tests/slab_allocator_test.cpp): size classes, chunk reuse, budget, minimum
  committed pages.
- [element_set_test.cpp](../../tests/element_set_test.cpp): `ElementSet` against a `std::map`.
- [collections_test.cpp](../../tests/collections_test.cpp): `CachedSmallCollectionsMatchRocksDB` and
  `LargeCollectionsAreNotCachedWhole`.
