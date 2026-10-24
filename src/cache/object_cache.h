#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "cache/admission.h"
#include "mem/page_arena.h"
#include "mem/size_classes.h"
#include "mem/slab_allocator.h"
#include "storage/record.h"
#include "util/hash.h"

namespace datadam {

struct CacheOptions {
  uint64_t max_memory = 512ULL << 20;  // pool ceiling; 0 disables the cache
  uint64_t min_memory = 64ULL << 20;   // idle eviction stops at this many cached bytes
  uint64_t max_object_size = 4096;     // key + value; larger objects are never cached
  int64_t max_idle_ms = 300'000;       // evict items not read for this long (0 = off)
  int64_t max_age_ms = 0;              // evict items cached for this long (0 = off)
  unsigned shards = 16;
  bool admission = true;               // when full, admit a key only on its second access
};

struct CacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t inserts = 0;
  uint64_t rejected_too_large = 0;
  uint64_t rejected_admission = 0;  // first-time keys turned away while the cache was full
  uint64_t alloc_failures = 0;
  uint64_t evicted_lru = 0;   // memory pressure, least recently used first
  uint64_t evicted_idle = 0;  // max-idle
  uint64_t evicted_age = 0;   // max-age
  uint64_t evicted_ttl = 0;   // key TTL reached
  uint64_t page_steals = 0;   // pages moved between size classes under pressure
  uint64_t items = 0;
  uint64_t chunk_bytes = 0;   // bytes of chunks in use (header + key + value + slack)
  uint64_t payload_bytes = 0; // key + value bytes
  uint64_t pages_in_use = 0;
  uint64_t pages_committed = 0;
  uint64_t page_size = 0;
};

struct CacheClassStats {
  size_t chunk_size = 0;
  size_t pages = 0;
  size_t items = 0;
};

// Sharded object cache whose items live in a slab memory pool. Each shard has
// its own lock, hash table, slab allocator and per-class LRU lists; pages come
// from one shared arena so memory flows to whichever shard/class needs it.
//
// Eviction policy:
//  - memory pressure: least recently used item (across size classes) goes first;
//  - max-idle: items not accessed for that long are released, but never below min-memory;
//  - max-age: items loaded longer ago than that are released;
//  - TTL: items whose key expired are released.
class ObjectCache {
 public:
  explicit ObjectCache(const CacheOptions& opts);
  ~ObjectCache();
  ObjectCache(const ObjectCache&) = delete;
  ObjectCache& operator=(const ObjectCache&) = delete;

  bool enabled() const { return arena_.max_pages() > 0; }
  bool cacheable(size_t key_len, size_t value_len) const {
    return enabled() && key_len <= UINT16_MAX && key_len + value_len <= opts_.max_object_size;
  }

  // On a hit calls fn(const RecordView&) while holding the shard lock; fn must
  // not call back into the cache. Returns false on a miss. An item that is
  // still pending (see insert) counts as a miss unless pending_ok: only the
  // writer, which holds the key's lock, may see it.
  template <class Fn>
  bool lookup(std::string_view key, Fn&& fn, bool count_stats = true, bool pending_ok = false) {
    uint64_t h = hash_bytes(key);
    Shard& sh = shard_for(h);
    std::lock_guard lock(sh.mu);
    Item* it = find_live(sh, h, key, count_stats, pending_ok);
    if (it == nullptr) return false;
    fn(it->view());
    return true;
  }

  // Inserts or replaces. Objects that are not cacheable remove any cached copy.
  // `admit` skips the admission filter (the key is known to be wanted).
  // `pending`: the write is not committed yet; lock-free readers do not see
  // the item until confirm(key).
  bool insert(std::string_view key, const RecordView& rec, bool admit = false, bool pending = false);
  // The write of a pending item was committed: everyone may see it now.
  void confirm(std::string_view key);
  // Whether the cache holds a pending item for key.
  bool pending(std::string_view key);
  // Milliseconds since the cached item of key was last read (nullopt: not cached).
  std::optional<int64_t> idle_ms(std::string_view key);
  void erase(std::string_view key);
  void clear();

  // Idle/age/TTL eviction over a bounded slice of each shard. Returns items evicted.
  size_t run_maintenance();

  CacheStats stats() const;
  std::vector<CacheClassStats> class_stats() const;
  // The options in effect (the eviction ones may have been changed since).
  CacheOptions options() const;
  // Changes the eviction settings while the cache is in use.
  void set_eviction(uint64_t min_memory, int64_t max_idle_ms, int64_t max_age_ms);
  void set_admission(bool on) { admission_.store(on, std::memory_order_relaxed); }
  // A lookup of `key` missed, but the read will look again once the key is
  // loaded: the miss is not counted twice.
  void defer_miss(std::string_view key);
  const SizeClasses& classes() const { return classes_; }
  size_t page_size() const { return arena_.page_size(); }

 private:
  struct Item {
    Item* lru_prev;
    Item* lru_next;
    Item* hnext;
    int64_t expire_at_ms;
    uint64_t cas;
    int64_t loaded_at;    // steady ms
    int64_t last_access;  // steady ms
    uint32_t hash;
    uint32_t flags;
    uint32_t value_len;
    uint16_t key_len;
    uint8_t type;  // ValueType, plus kPendingBit
    uint8_t cls;

    static constexpr uint8_t kPendingBit = 0x80;
    bool pending() const { return (type & kPendingBit) != 0; }
    char* data() { return reinterpret_cast<char*>(this + 1); }
    const char* data() const { return reinterpret_cast<const char*>(this + 1); }
    std::string_view key() const { return {data(), key_len}; }
    RecordView view() const {
      return {static_cast<ValueType>(type & ~kPendingBit), flags, expire_at_ms, cas, {data() + key_len, value_len}};
    }
  };

  struct Lru {
    Item* head = nullptr;
    Item* tail = nullptr;
    size_t count = 0;
  };

  struct Shard {
    mutable std::mutex mu;
    std::vector<Item*> buckets;
    size_t items = 0;
    uint64_t payload_bytes = 0;
    std::unique_ptr<SlabAllocator> slab;
    Lru lru[SizeClasses::kMaxClasses];
    size_t crawl_cursor = 0;
    CacheStats stats;
  };

  static size_t compute_page_size(size_t max_chunk);

  Shard& shard_for(uint64_t h) { return *shards_[(h >> 32) % shards_.size()]; }
  Item* find(Shard& sh, uint64_t h, std::string_view key);
  Item* find_live(Shard& sh, uint64_t h, std::string_view key, bool count_stats, bool pending_ok);
  Item* oldest(Shard& sh);
  void* allocate(Shard& sh, size_t cls);
  void remove(Shard& sh, Item* it);
  void lru_unlink(Shard& sh, Item* it);
  void lru_push_front(Shard& sh, Item* it);
  void grow(Shard& sh);

  CacheOptions opts_;  // fixed settings; the eviction ones live in the atomics below
  std::atomic<uint64_t> min_memory_;
  std::atomic<int64_t> max_idle_ms_;
  std::atomic<int64_t> max_age_ms_;
  std::atomic<bool> admission_;
  std::unique_ptr<Doorkeeper> door_;
  SizeClasses classes_;
  PageArena arena_;
  std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace datadam
