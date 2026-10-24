#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <random>
#include <thread>

#include "cache/object_cache.h"
#include "db/database.h"
#include "storage/rocks_storage.h"
#include "util/clock.h"

using namespace datadam;
namespace fs = std::filesystem;

namespace {

class DatabaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_test_" + std::to_string(std::random_device{}()));
    open();
  }
  void TearDown() override {
    close();
    fs::remove_all(dir_);
  }

  void open() {
    StorageOptions so;
    so.dir = dir_.string();
    so.fsync = FsyncPolicy::No;
    so.block_cache = 8 << 20;
    so.write_buffer = 4 << 20;
    storage_ = RocksStorage::open(so);
    CacheOptions co;
    co.max_memory = 4 << 20;
    co.min_memory = 0;
    co.shards = 2;
    cache_ = std::make_unique<ObjectCache>(co);
    db_ = std::make_unique<Database>(*storage_, *cache_);
  }
  void close() {
    db_.reset();
    cache_.reset();
    storage_.reset();
  }

  std::string value(std::string_view key) {
    auto r = db_->get(key);
    return r ? r->value : "<none>";
  }

  fs::path dir_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
};

Record str(std::string v, int64_t expire_at = 0) {
  Record r;
  r.value = std::move(v);
  r.expire_at_ms = expire_at;
  return r;
}

}  // namespace

TEST_F(DatabaseTest, PutGetDelete) {
  db_->put("k", str("v"));
  EXPECT_EQ(value("k"), "v");
  EXPECT_TRUE(db_->exists("k"));
  EXPECT_TRUE(db_->del("k"));
  EXPECT_FALSE(db_->del("k"));
  EXPECT_EQ(value("k"), "<none>");
}

TEST_F(DatabaseTest, PersistsAcrossRestart) {
  db_->put("durable", str("yes"));
  std::string big(100'000, 'b');  // larger than the cache's object limit
  db_->put("big", str(big));
  close();
  open();
  EXPECT_EQ(value("durable"), "yes");
  EXPECT_EQ(value("big"), big);
  EXPECT_EQ(cache_->stats().items, 1u);  // only the small one was cached on read
}

TEST_F(DatabaseTest, ReadsComeFromCacheAfterFirstLoad) {
  db_->put("k", str("v"));
  cache_->clear();
  EXPECT_EQ(value("k"), "v");  // miss -> RocksDB -> cache
  auto before = cache_->stats().hits;
  EXPECT_EQ(value("k"), "v");
  EXPECT_EQ(cache_->stats().hits, before + 1);
}

TEST_F(DatabaseTest, UpdateIsReadModifyWrite) {
  for (int i = 0; i < 10; ++i) {
    db_->update("counter", [](std::optional<Record>& rec) {
      int n = rec ? std::stoi(rec->value) : 0;
      if (!rec) rec.emplace();
      rec->value = std::to_string(n + 1);
      return Database::Op::Put;
    });
  }
  EXPECT_EQ(value("counter"), "10");
}

TEST_F(DatabaseTest, CasChangesOnWriteButNotOnTouch) {
  db_->put("k", str("v"));
  uint64_t cas1 = db_->get("k")->cas;
  db_->update("k", [](std::optional<Record>& rec) {
    rec->expire_at_ms = unix_time_ms() + 60'000;
    return Database::Op::PutKeepCas;
  });
  EXPECT_EQ(db_->get("k")->cas, cas1);
  db_->put("k", str("v2"));
  EXPECT_NE(db_->get("k")->cas, cas1);
}

TEST_F(DatabaseTest, ExpiredKeysDisappear) {
  db_->put("k", str("v", unix_time_ms() + 30));
  EXPECT_EQ(value("k"), "v");
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_EQ(value("k"), "<none>");
  cache_->clear();
  EXPECT_EQ(value("k"), "<none>");  // also gone from disk
}

TEST_F(DatabaseTest, MsetRenameKeysFlush) {
  db_->mset({{"a", "1"}, {"b", "2"}}, false);
  EXPECT_FALSE(db_->mset({{"b", "x"}, {"c", "3"}}, true));
  EXPECT_EQ(value("c"), "<none>");
  EXPECT_EQ(db_->rename("a", "z", false), Database::RenameResult::Ok);
  EXPECT_EQ(value("a"), "<none>");
  EXPECT_EQ(value("z"), "1");
  EXPECT_EQ(db_->rename("missing", "q", false), Database::RenameResult::NoSuchKey);
  EXPECT_EQ(db_->rename("z", "b", true), Database::RenameResult::TargetExists);
  auto keys = db_->keys("*");
  EXPECT_EQ(keys.size(), 2u);
  db_->flush_all();
  EXPECT_TRUE(db_->keys("*").empty());
  EXPECT_EQ(value("b"), "<none>");
}

TEST_F(DatabaseTest, BatchReadsItsOwnWritesAndPersists) {
  std::string big(10'000, 'x');  // not cacheable: reads must come from the pending batch
  {
    Database::WriteBatchScope batch(*db_);
    db_->put("big", str(big));
    db_->put("small", str("s"));
    EXPECT_EQ(value("big"), big);
    EXPECT_TRUE(db_->exists("big"));
    EXPECT_TRUE(db_->del("small"));
    EXPECT_FALSE(db_->exists("small"));
    EXPECT_EQ(db_->keys("*"), std::vector<std::string>{"big"});  // KEYS commits first
    db_->put("after", str(big));
    batch.commit();
  }
  EXPECT_GE(db_->write_batch_stats().commits, 2u);
  close();
  open();
  EXPECT_EQ(value("big"), big);
  EXPECT_EQ(value("after"), big);
  EXPECT_EQ(value("small"), "<none>");
}

TEST_F(DatabaseTest, BatchHoldsKeyUntilCommit) {
  std::string big(10'000, 'n');
  db_->put("k", str("old"));
  cache_->clear();
  std::atomic<bool> read_done{false};
  std::string seen;
  {
    Database::WriteBatchScope batch(*db_);
    db_->put("k", str(big));
    std::thread reader([&] {
      seen = value("k");  // must wait for the commit, never return "old"
      read_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(read_done.load());
    batch.commit();
    reader.join();
  }
  EXPECT_EQ(seen, big);
}

TEST_F(DatabaseTest, LoadManyIntoCacheWithOneMultiGet) {
  db_->put("a", str("1"));
  db_->put("b", str("2"));
  db_->put("big", str(std::string(10'000, 'x')));  // too large for the cache
  db_->put("old", str("o", unix_time_ms() + 20));
  db_->put("busy", str("3"));
  std::this_thread::sleep_for(std::chrono::milliseconds(40));  // "old" expires
  cache_->clear();
  db_->put("hot", str("4"));  // already cached
  // Another client holds the lock of "busy" for a moment: it is loaded after the others.
  std::promise<void> locked;
  std::thread holder([&] {
    Database::WriteBatchScope batch(*db_);
    db_->update("busy", [](std::optional<Record>&) { return Database::Op::Keep; });
    cache_->erase("busy");
    locked.set_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    batch.commit();
  });
  locked.get_future().wait();
  auto gets = storage_->multi_get_latency().count();
  std::vector<std::string> keys = {"a", "missing", "b", "big", "old", "hot", "busy", "a"};
  std::vector<bool> got = db_->load_many_into_cache(keys);
  holder.join();
  EXPECT_EQ(got, (std::vector<bool>{true, false, true, false, false, true, true, true}));
  EXPECT_EQ(storage_->multi_get_latency().count(), gets + 1);  // one MultiGet for all the unlocked ones
  auto before = cache_->stats().hits;
  for (std::string_view k : {"a", "b", "hot", "busy"}) EXPECT_NE(value(k), "<none>");
  EXPECT_EQ(cache_->stats().hits, before + 4);  // all four from the cache
  EXPECT_EQ(value("big"), std::string(10'000, 'x'));
  EXPECT_EQ(value("old"), "<none>");
}

TEST_F(DatabaseTest, BatchDoesNotHoldKeysWhileReadingDisk) {
  db_->put("cold", str("c"));
  cache_->clear();
  std::string big(10'000, 'w');
  {
    Database::WriteBatchScope batch(*db_);
    db_->put("w", str(big));  // not cacheable: held by the batch
    auto commits = db_->write_batch_stats().commits;
    EXPECT_EQ(value("cold"), "c");  // a cache miss: the batch commits before the disk read
    EXPECT_EQ(db_->write_batch_stats().commits, commits + 1);
    // So "w" is free: another client reads it without waiting for this batch.
    auto other = std::async(std::launch::async, [&] { return value("w"); });
    EXPECT_EQ(other.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    // A cached key read inside the batch commits nothing.
    db_->put("w2", str("x"));
    EXPECT_EQ(value("cold"), "c");
    EXPECT_EQ(db_->write_batch_stats().commits, commits + 1);
    batch.commit();
    EXPECT_EQ(other.get(), big);
  }
}

TEST_F(DatabaseTest, ConcurrentBatchesAreAtomicAndDeadlockFree) {
  constexpr int kThreads = 4, kRounds = 2000, kPerBatch = 6, kKeys = 32;
  auto incr = [&](const std::string& key) {
    db_->update(key, [](std::optional<Record>& rec) {
      int n = rec ? std::stoi(rec->value) : 0;
      if (!rec) rec.emplace();
      rec->value = std::to_string(n + 1);
      return Database::Op::Put;
    });
  };
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(t);
      for (int r = 0; r < kRounds; ++r) {
        Database::WriteBatchScope batch(*db_);
        for (int i = 0; i < kPerBatch; ++i) incr("c" + std::to_string(rng() % kKeys));
        if (r % 100 == 0) db_->mset({{"m" + std::to_string(t), "x"}}, false);  // multi-key inside a batch
        batch.commit();
      }
    });
  }
  for (auto& th : threads) th.join();
  auto check = [&] {
    long total = 0;
    for (int k = 0; k < kKeys; ++k) {
      std::string v = value("c" + std::to_string(k));
      if (v != "<none>") total += std::stol(v);
    }
    EXPECT_EQ(total, long{kThreads} * kRounds * kPerBatch);
  };
  check();
  cache_->clear();
  check();  // same totals from disk
}

TEST_F(DatabaseTest, BatchKeepsOnlyTheLastWriteOfAKey) {
  {
    Database::WriteBatchScope batch(*db_);
    db_->put("k", str("1"));
    EXPECT_TRUE(db_->del("k"));
    db_->put("k", str("2"));
    db_->put("gone", str("x"));
    EXPECT_TRUE(db_->del("gone"));
    for (int i = 0; i < 100; ++i) {
      db_->update("n", [](std::optional<Record>& rec) {
        int n = rec ? std::stoi(rec->value) : 0;
        if (!rec) rec.emplace();
        rec->value = std::to_string(n + 1);
        return Database::Op::Put;
      });
    }
    batch.commit();
  }
  EXPECT_EQ(db_->write_batch_stats().writes, 105u);  // counted before coalescing
  close();
  open();
  EXPECT_EQ(value("k"), "2");
  EXPECT_EQ(value("gone"), "<none>");
  EXPECT_EQ(value("n"), "100");
}
