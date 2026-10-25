#include <gtest/gtest.h>

#include <chrono>
#include <random>
#include <string>
#include <thread>

#include "cache/object_cache.h"
#include "util/clock.h"

using namespace datadam;

namespace {

CacheOptions small_cache(uint64_t max_memory) {
  CacheOptions o;
  o.max_memory = max_memory;
  o.min_memory = 0;
  o.max_object_size = 4096;
  o.max_idle_ms = 0;
  o.max_age_ms = 0;
  o.shards = 1;
  o.admission = false;  // these tests are about eviction; admission has its own
  return o;
}

RecordView rec(std::string_view value) {
  RecordView r;
  r.value = value;
  return r;
}

std::string get(ObjectCache& c, std::string_view key) {
  std::string out = "<miss>";
  c.lookup(key, [&](const RecordView& v) { out = std::string(v.value); });
  return out;
}

}  // namespace

TEST(ObjectCache, InsertLookupErase) {
  ObjectCache c(small_cache(1 << 20));
  EXPECT_TRUE(c.insert("a", rec("1")));
  EXPECT_TRUE(c.insert("b", rec("2")));
  EXPECT_EQ(get(c, "a"), "1");
  EXPECT_TRUE(c.insert("a", rec("updated")));
  EXPECT_EQ(get(c, "a"), "updated");
  c.erase("a");
  EXPECT_EQ(get(c, "a"), "<miss>");
  auto s = c.stats();
  EXPECT_EQ(s.items, 1u);
  EXPECT_EQ(s.hits, 2u);
  EXPECT_EQ(s.misses, 1u);
}

TEST(ObjectCache, LargeObjectsAreNotCached) {
  ObjectCache c(small_cache(1 << 20));
  std::string big(5000, 'x');
  EXPECT_TRUE(c.insert("k", rec("small")));
  EXPECT_FALSE(c.insert("k", rec(big)));
  EXPECT_EQ(get(c, "k"), "<miss>");  // the stale small copy is gone too
  EXPECT_EQ(c.stats().rejected_too_large, 1u);
}

TEST(ObjectCache, EvictsLeastRecentlyUsedUnderPressure) {
  ObjectCache c(small_cache(2 * 64 * 1024));  // two pages
  std::string v(1000, 'v');
  c.insert("hot", rec(v));
  for (int i = 0; i < 1000; ++i) {
    c.insert("k" + std::to_string(i), rec(v));
    get(c, "hot");  // keep it recently used
  }
  auto s = c.stats();
  EXPECT_GT(s.evicted_lru, 0u);
  EXPECT_LE(s.pages_in_use, 2u);
  EXPECT_EQ(get(c, "hot"), v);
  EXPECT_EQ(get(c, "k0"), "<miss>");
  EXPECT_EQ(get(c, "k999"), v);
}

TEST(ObjectCache, StealsPagesAcrossSizeClasses) {
  ObjectCache c(small_cache(2 * 64 * 1024));
  for (int i = 0; i < 200; ++i) c.insert("small" + std::to_string(i), rec(std::string(500, 's')));
  // Fill with a different size class; the old small-class pages must be reused.
  for (int i = 0; i < 200; ++i) c.insert("large" + std::to_string(i), rec(std::string(3000, 'l')));
  auto s = c.stats();
  EXPECT_GT(s.page_steals, 0u);
  EXPECT_EQ(get(c, "large199"), std::string(3000, 'l'));
}

TEST(ObjectCache, IdleEvictionRespectsMinimum) {
  CacheOptions o = small_cache(1 << 20);
  o.max_idle_ms = 20;
  ObjectCache c(o);
  for (int i = 0; i < 50; ++i) c.insert("k" + std::to_string(i), rec("value"));
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_EQ(c.run_maintenance(), 50u);
  EXPECT_EQ(c.stats().items, 0u);
  EXPECT_EQ(c.stats().evicted_idle, 50u);

  o.min_memory = 1 << 20;  // the minimum covers everything: nothing is evicted
  ObjectCache keep(o);
  for (int i = 0; i < 50; ++i) keep.insert("k" + std::to_string(i), rec("value"));
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_EQ(keep.run_maintenance(), 0u);
  EXPECT_EQ(keep.stats().items, 50u);
}

TEST(ObjectCache, MaxAgeExpiresEvenHotItems) {
  CacheOptions o = small_cache(1 << 20);
  o.max_age_ms = 20;
  ObjectCache c(o);
  c.insert("k", rec("v"));
  EXPECT_EQ(get(c, "k"), "v");
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_EQ(get(c, "k"), "<miss>");
  EXPECT_EQ(c.stats().evicted_age, 1u);
}

TEST(ObjectCache, ExpiredTtlIsAMiss) {
  ObjectCache c(small_cache(1 << 20));
  RecordView r = rec("v");
  r.expire_at_ms = unix_time_ms() + 20;
  c.insert("k", r);
  EXPECT_EQ(get(c, "k"), "v");
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_EQ(get(c, "k"), "<miss>");
}

TEST(ObjectCache, DisabledWhenBudgetIsZero) {
  ObjectCache c(small_cache(0));
  EXPECT_FALSE(c.enabled());
  EXPECT_FALSE(c.insert("k", rec("v")));
  EXPECT_EQ(get(c, "k"), "<miss>");
}

TEST(ObjectCache, FullCacheAdmitsKeysOnTheirSecondAccess) {
  CacheOptions o = small_cache(16 * 64 * 1024);  // 16 pages, about 900 items of 1 KB
  o.admission = true;
  ObjectCache c(o);
  std::string v(1000, 'v');
  // While there is room, everything gets in.
  for (int i = 0; i < 50; ++i) c.insert("hot" + std::to_string(i), rec(v));
  EXPECT_EQ(c.stats().rejected_admission, 0u);
  // Full: a stream of one-off keys is turned away and the hot keys stay.
  for (int i = 0; i < 4000; ++i) c.insert("once" + std::to_string(i), rec(v));
  EXPECT_GT(c.stats().rejected_admission, 2500u);
  int hot_left = 0;
  for (int i = 0; i < 50; ++i) hot_left += get(c, "hot" + std::to_string(i)) == v;
  EXPECT_EQ(hot_left, 50);
  // A key offered twice gets in, evicting the least recently used.
  EXPECT_FALSE(c.insert("again", rec(v)));
  EXPECT_TRUE(c.insert("again", rec(v)));
  EXPECT_EQ(get(c, "again"), v);
  // A cached key is always replaced (never left stale).
  EXPECT_TRUE(c.insert("hot1", rec("new")));
  EXPECT_EQ(get(c, "hot1"), "new");
  // Admission can be switched off at runtime.
  c.set_admission(false);
  EXPECT_TRUE(c.insert("first-time", rec(v)));
}

// Leak check of the pool: after any mix of inserts, replacements, erases,
// pending items, evictions under pressure and page steals between size
// classes, every chunk and page comes back when the cache is emptied. Every
// value read back must be one that was written to that key (ASan builds also
// poison freed chunks, so a read of one would be reported).
TEST(ObjectCache, PoolGetsEveryChunkBack) {
  CacheOptions o = small_cache(2 << 20);
  o.shards = 4;
  ObjectCache c(o);
  std::mt19937_64 rng(7);
  std::string value;
  for (int round = 0; round < 4; ++round) {
    for (int i = 0; i < 30000; ++i) {
      std::string key = "k" + std::to_string(rng() % 4000);
      uint64_t op = rng() % 10;
      if (op < 6) {
        // Sizes spread over many classes, so pressure moves pages between them.
        value.assign(rng() % (round % 2 == 0 ? 3500 : 300), static_cast<char>('a' + key.size() % 26));
        bool pending = rng() % 8 == 0;
        c.insert(key, rec(value), false, pending);
        if (pending) c.confirm(key);
      } else if (op < 8) {
        c.erase(key);
      } else {
        c.lookup(key, [&](const RecordView& v) {
          EXPECT_TRUE(v.value.empty() || v.value[0] == static_cast<char>('a' + key.size() % 26)) << key;
        });
      }
    }
    CacheStats s = c.stats();
    EXPECT_LE(s.pages_in_use * c.page_size(), o.max_memory) << "round " << round;
    EXPECT_EQ(s.items == 0, s.chunk_bytes == 0) << "round " << round;
  }
  c.clear();
  CacheStats s = c.stats();
  EXPECT_EQ(s.items, 0u);
  EXPECT_EQ(s.chunk_bytes, 0u);
  EXPECT_EQ(s.payload_bytes, 0u);
  EXPECT_EQ(s.pages_in_use, 0u);
  for (const auto& cls : c.class_stats()) {
    EXPECT_EQ(cls.items, 0u) << "chunk size " << cls.chunk_size;
    EXPECT_EQ(cls.pages, 0u) << "chunk size " << cls.chunk_size;
  }
}
