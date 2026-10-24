#include "cache/object_cache.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

#include "util/clock.h"

namespace datadam {

namespace {
constexpr size_t kInitialBuckets = 1024;
constexpr size_t kMaxIdleEvictionsPerPass = 10'000;
constexpr size_t kCrawlBucketsPerPass = 4096;
constexpr int kMaxAllocAttempts = 16;
}  // namespace

size_t ObjectCache::compute_page_size(size_t max_chunk) {
  // Small pages keep a page steal cheap; at least four of the largest chunks per page.
  return std::max<size_t>(64 * 1024, std::bit_ceil(max_chunk) * 4);
}

ObjectCache::ObjectCache(const CacheOptions& opts)
    : opts_(opts),
      min_memory_(opts.min_memory),
      max_idle_ms_(opts.max_idle_ms),
      max_age_ms_(opts.max_age_ms),
      admission_(opts.admission),
      // The window: about as many keys as the cache holds (512 B per item).
      door_(std::make_unique<Doorkeeper>(opts.max_memory / 512)),
      classes_(sizeof(Item) + 16, sizeof(Item) + opts.max_object_size),
      arena_(opts.max_memory, opts.min_memory, compute_page_size(classes_.max_chunk()), classes_.min_chunk()) {
  static_assert(sizeof(Item) == 72, "keep the item header compact");
  unsigned n = std::clamp(opts_.shards, 1u, 1024u);
  shards_.reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    auto sh = std::make_unique<Shard>();
    sh->buckets.assign(kInitialBuckets, nullptr);
    sh->slab = std::make_unique<SlabAllocator>(arena_, classes_, static_cast<uint16_t>(i));
    shards_.push_back(std::move(sh));
  }
}

ObjectCache::~ObjectCache() = default;

ObjectCache::Item* ObjectCache::find(Shard& sh, uint64_t h, std::string_view key) {
  uint32_t h32 = static_cast<uint32_t>(h);
  for (Item* it = sh.buckets[h32 & (sh.buckets.size() - 1)]; it != nullptr; it = it->hnext) {
    if (it->hash == h32 && it->key_len == key.size() && std::memcmp(it->data(), key.data(), key.size()) == 0) {
      return it;
    }
  }
  return nullptr;
}

ObjectCache::Item* ObjectCache::find_live(Shard& sh, uint64_t h, std::string_view key, bool count_stats,
                                          bool pending_ok) {
  Item* it = find(sh, h, key);
  if (it != nullptr && it->pending() && !pending_ok) {
    it = nullptr;  // not committed yet: only its writer may read it
  } else if (it != nullptr) {
    int64_t now = steady_time_ms();
    int64_t max_age = max_age_ms_.load(std::memory_order_relaxed);
    if (max_age > 0 && now - it->loaded_at >= max_age) {
      remove(sh, it);
      ++sh.stats.evicted_age;
      it = nullptr;
    } else if (it->expire_at_ms != 0 && unix_time_ms() >= it->expire_at_ms) {
      remove(sh, it);
      ++sh.stats.evicted_ttl;
      it = nullptr;
    } else {
      it->last_access = now;
      if (sh.lru[it->cls].head != it) {
        lru_unlink(sh, it);
        lru_push_front(sh, it);
      }
    }
  }
  if (count_stats) {
    if (it != nullptr) ++sh.stats.hits;
    else ++sh.stats.misses;
  }
  return it;
}

void ObjectCache::lru_unlink(Shard& sh, Item* it) {
  Lru& l = sh.lru[it->cls];
  if (it->lru_prev != nullptr) it->lru_prev->lru_next = it->lru_next;
  else l.head = it->lru_next;
  if (it->lru_next != nullptr) it->lru_next->lru_prev = it->lru_prev;
  else l.tail = it->lru_prev;
  it->lru_prev = it->lru_next = nullptr;
  --l.count;
}

void ObjectCache::lru_push_front(Shard& sh, Item* it) {
  Lru& l = sh.lru[it->cls];
  it->lru_prev = nullptr;
  it->lru_next = l.head;
  if (l.head != nullptr) l.head->lru_prev = it;
  l.head = it;
  if (l.tail == nullptr) l.tail = it;
  ++l.count;
}

ObjectCache::Item* ObjectCache::oldest(Shard& sh) {
  Item* best = nullptr;
  for (size_t c = 0; c < classes_.count(); ++c) {
    Item* t = sh.lru[c].tail;
    if (t != nullptr && (best == nullptr || t->last_access < best->last_access)) best = t;
  }
  return best;
}

void ObjectCache::remove(Shard& sh, Item* it) {
  Item** pp = &sh.buckets[it->hash & (sh.buckets.size() - 1)];
  while (*pp != it) pp = &(*pp)->hnext;
  *pp = it->hnext;
  lru_unlink(sh, it);
  --sh.items;
  sh.payload_bytes -= it->key_len + it->value_len;
  it->~Item();
  sh.slab->deallocate(it);
}

void* ObjectCache::allocate(Shard& sh, size_t cls) {
  for (int attempt = 0; attempt < kMaxAllocAttempts; ++attempt) {
    if (void* p = sh.slab->allocate(cls)) return p;

    // The pool is full: release the least recently used memory.
    Item* victim = oldest(sh);
    if (victim == nullptr) return nullptr;  // the other shards hold every page
    Item* own = sh.lru[cls].tail;
    int64_t now = steady_time_ms();
    if (own != nullptr && (own == victim || (now - own->last_access) * 2 >= now - victim->last_access)) {
      // Evicting within the same class frees a chunk of exactly the right size.
      remove(sh, own);
      ++sh.stats.evicted_lru;
      continue;
    }
    // The oldest data is in another class: empty its page so it returns to the
    // arena and can be re-carved for this class.
    for (void* chunk : sh.slab->live_chunks(sh.slab->page_of(victim))) {
      remove(sh, static_cast<Item*>(chunk));
      ++sh.stats.evicted_lru;
    }
    ++sh.stats.page_steals;
  }
  return nullptr;
}

void ObjectCache::grow(Shard& sh) {
  std::vector<Item*> next(sh.buckets.size() * 2, nullptr);
  size_t mask = next.size() - 1;
  for (Item* head : sh.buckets) {
    while (head != nullptr) {
      Item* n = head->hnext;
      Item*& slot = next[head->hash & mask];
      head->hnext = slot;
      slot = head;
      head = n;
    }
  }
  sh.buckets.swap(next);
}

bool ObjectCache::insert(std::string_view key, const RecordView& rec, bool admit, bool pending) {
  uint64_t h = hash_bytes(key);
  Shard& sh = shard_for(h);
  std::lock_guard lock(sh.mu);

  bool replacing = false;
  if (Item* old = find(sh, h, key)) {
    remove(sh, old);
    replacing = true;
  }
  if (!cacheable(key.size(), rec.value.size())) {
    ++sh.stats.rejected_too_large;
    return false;
  }
  if (rec.expired(unix_time_ms())) return false;
  // A full cache takes a new key only on its second access: one-off reads
  // and writes would otherwise push out the keys that are really hot. A key
  // already cached is replaced as usual (it earned its place).
  if (!replacing && !admit && admission_.load(std::memory_order_relaxed) &&
      arena_.pages_in_use() + shards_.size() >= arena_.max_pages() && !door_->seen_or_add(h)) {
    ++sh.stats.rejected_admission;
    return false;
  }

  int cls = classes_.class_for(sizeof(Item) + key.size() + rec.value.size());
  void* mem = allocate(sh, static_cast<size_t>(cls));
  if (mem == nullptr) {
    ++sh.stats.alloc_failures;
    return false;
  }

  int64_t now = steady_time_ms();
  Item* it = new (mem) Item{};
  it->expire_at_ms = rec.expire_at_ms;
  it->cas = rec.cas;
  it->loaded_at = now;
  it->last_access = now;
  it->hash = static_cast<uint32_t>(h);
  it->flags = rec.flags;
  it->value_len = static_cast<uint32_t>(rec.value.size());
  it->key_len = static_cast<uint16_t>(key.size());
  it->type = static_cast<uint8_t>(static_cast<uint8_t>(rec.type) | (pending ? Item::kPendingBit : 0));
  it->cls = static_cast<uint8_t>(cls);
  std::memcpy(it->data(), key.data(), key.size());
  if (!rec.value.empty()) std::memcpy(it->data() + key.size(), rec.value.data(), rec.value.size());

  Item*& slot = sh.buckets[it->hash & (sh.buckets.size() - 1)];
  it->hnext = slot;
  slot = it;
  lru_push_front(sh, it);
  ++sh.items;
  sh.payload_bytes += key.size() + rec.value.size();
  ++sh.stats.inserts;
  if (sh.items > sh.buckets.size()) grow(sh);
  return true;
}

void ObjectCache::defer_miss(std::string_view key) {
  uint64_t h = hash_bytes(key);
  Shard& sh = shard_for(h);
  std::lock_guard lock(sh.mu);
  if (sh.stats.misses > 0) --sh.stats.misses;
}

void ObjectCache::confirm(std::string_view key) {
  uint64_t h = hash_bytes(key);
  Shard& sh = shard_for(h);
  std::lock_guard lock(sh.mu);
  if (Item* it = find(sh, h, key)) it->type &= static_cast<uint8_t>(~Item::kPendingBit);
}

bool ObjectCache::pending(std::string_view key) {
  uint64_t h = hash_bytes(key);
  Shard& sh = shard_for(h);
  std::lock_guard lock(sh.mu);
  Item* it = find(sh, h, key);
  return it != nullptr && it->pending();
}

std::optional<int64_t> ObjectCache::idle_ms(std::string_view key) {
  uint64_t h = hash_bytes(key);
  Shard& sh = shard_for(h);
  std::lock_guard lock(sh.mu);
  Item* it = find(sh, h, key);
  if (it == nullptr) return std::nullopt;
  return steady_time_ms() - it->last_access;
}

void ObjectCache::erase(std::string_view key) {
  uint64_t h = hash_bytes(key);
  Shard& sh = shard_for(h);
  std::lock_guard lock(sh.mu);
  if (Item* it = find(sh, h, key)) remove(sh, it);
}

void ObjectCache::clear() {
  for (auto& shp : shards_) {
    Shard& sh = *shp;
    std::lock_guard lock(sh.mu);
    for (Item*& head : sh.buckets) {
      while (head != nullptr) remove(sh, head);
    }
  }
}

size_t ObjectCache::run_maintenance() {
  size_t evicted = 0;
  size_t shard_min = min_memory_.load(std::memory_order_relaxed) / shards_.size();
  int64_t max_idle = max_idle_ms_.load(std::memory_order_relaxed);
  int64_t max_age = max_age_ms_.load(std::memory_order_relaxed);
  for (auto& shp : shards_) {
    Shard& sh = *shp;
    std::lock_guard lock(sh.mu);
    int64_t now = steady_time_ms();
    int64_t unix_now = unix_time_ms();

    // Idle items, least recently used first, keeping at least min-memory cached.
    if (max_idle > 0) {
      for (size_t n = 0; n < kMaxIdleEvictionsPerPass && sh.slab->bytes_allocated() > shard_min; ++n) {
        Item* victim = oldest(sh);
        if (victim == nullptr || now - victim->last_access < max_idle) break;
        remove(sh, victim);
        ++sh.stats.evicted_idle;
        ++evicted;
      }
    }

    // Walk a slice of the hash table for items past max-age or their TTL.
    size_t buckets = std::min(sh.buckets.size(), kCrawlBucketsPerPass);
    for (size_t i = 0; i < buckets; ++i) {
      Item** pp = &sh.buckets[sh.crawl_cursor++ & (sh.buckets.size() - 1)];
      while (*pp != nullptr) {
        Item* it = *pp;
        bool too_old = max_age > 0 && now - it->loaded_at >= max_age;
        bool expired = it->expire_at_ms != 0 && unix_now >= it->expire_at_ms;
        if (too_old || expired) {
          remove(sh, it);  // unlinks *pp, which now points at the next item
          ++(too_old ? sh.stats.evicted_age : sh.stats.evicted_ttl);
          ++evicted;
        } else {
          pp = &it->hnext;
        }
      }
    }
  }
  return evicted;
}

CacheStats ObjectCache::stats() const {
  CacheStats total;
  for (const auto& shp : shards_) {
    const Shard& sh = *shp;
    std::lock_guard lock(sh.mu);
    const CacheStats& s = sh.stats;
    total.hits += s.hits;
    total.misses += s.misses;
    total.inserts += s.inserts;
    total.rejected_too_large += s.rejected_too_large;
    total.rejected_admission += s.rejected_admission;
    total.alloc_failures += s.alloc_failures;
    total.evicted_lru += s.evicted_lru;
    total.evicted_idle += s.evicted_idle;
    total.evicted_age += s.evicted_age;
    total.evicted_ttl += s.evicted_ttl;
    total.page_steals += s.page_steals;
    total.items += sh.items;
    total.chunk_bytes += sh.slab->bytes_allocated();
    total.payload_bytes += sh.payload_bytes;
  }
  total.pages_in_use = arena_.pages_in_use();
  total.pages_committed = arena_.pages_committed();
  total.page_size = arena_.page_size();
  return total;
}

std::vector<CacheClassStats> ObjectCache::class_stats() const {
  std::vector<CacheClassStats> out(classes_.count());
  for (size_t c = 0; c < out.size(); ++c) out[c].chunk_size = classes_.size(c);
  for (const auto& shp : shards_) {
    const Shard& sh = *shp;
    std::lock_guard lock(sh.mu);
    for (size_t c = 0; c < out.size(); ++c) {
      out[c].pages += sh.slab->pages_in_class(c);
      out[c].items += sh.lru[c].count;
    }
  }
  return out;
}

CacheOptions ObjectCache::options() const {
  CacheOptions o = opts_;
  o.min_memory = min_memory_.load(std::memory_order_relaxed);
  o.max_idle_ms = max_idle_ms_.load(std::memory_order_relaxed);
  o.max_age_ms = max_age_ms_.load(std::memory_order_relaxed);
  o.admission = admission_.load(std::memory_order_relaxed);
  return o;
}

void ObjectCache::set_eviction(uint64_t min_memory, int64_t max_idle_ms, int64_t max_age_ms) {
  min_memory_.store(min_memory, std::memory_order_relaxed);
  max_idle_ms_.store(max_idle_ms, std::memory_order_relaxed);
  max_age_ms_.store(max_age_ms, std::memory_order_relaxed);
}

}  // namespace datadam
