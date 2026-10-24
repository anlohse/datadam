#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <thread>

#include "cache/object_cache.h"
#include "db/database.h"
#include "server/redis_commands.h"
#include "storage/data_keys.h"
#include "storage/rocks_storage.h"

using namespace datadam;
namespace fs = std::filesystem;

namespace {

// Runs Redis commands straight through execute_redis_command (RESP2 replies).
class CollectionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_coll_" + std::to_string(std::random_device{}()));
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
    ctx_ = std::make_unique<ServerContext>(*db_, config_, stats_, 0);
  }
  void close() {
    ctx_.reset();
    db_.reset();
    cache_.reset();
    storage_.reset();
  }

  // Executes one command and returns its raw RESP reply.
  std::string run(std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, 2);
    execute_redis_command(*ctx_, client_, views, reply);
    return out;
  }

  // Number of entries in the data column family.
  size_t data_entries() {
    auto c = storage_->cursor(Cf::Data, "", std::string(32, '\xff'));
    size_t n = 0;
    for (c->seek_first(); c->valid(); c->next()) ++n;
    return n;
  }

  uint64_t version_of(const std::string& key) {
    auto rec = db_->get(key);
    CollectionMeta m;
    if (!rec || !decode_collection_meta(rec->value, &m)) return 0;
    return m.version;
  }

  fs::path dir_;
  Config config_;
  ServerStats stats_;
  ClientState client_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ServerContext> ctx_;
};

std::string bulk(std::string_view s) { return "$" + std::to_string(s.size()) + "\r\n" + std::string(s) + "\r\n"; }
std::string integer(int64_t v) { return ":" + std::to_string(v) + "\r\n"; }
std::string array(std::initializer_list<std::string> items) {
  std::string out = "*" + std::to_string(items.size()) + "\r\n";
  for (const auto& i : items) out += i;
  return out;
}

}  // namespace

TEST_F(CollectionsTest, AllTypesPersistAcrossRestart) {
  EXPECT_EQ(run({"HSET", "h", "f1", "v1", "f2", "v2"}), integer(2));
  EXPECT_EQ(run({"SADD", "s", "b", "a", "b"}), integer(2));
  EXPECT_EQ(run({"ZADD", "z", "2", "two", "1", "one", "3", "three"}), integer(3));
  EXPECT_EQ(run({"RPUSH", "l", "x", "y"}), integer(2));
  EXPECT_EQ(run({"LPUSH", "l", "w"}), integer(3));
  close();
  open();
  EXPECT_EQ(run({"HGETALL", "h"}), array({bulk("f1"), bulk("v1"), bulk("f2"), bulk("v2")}));
  EXPECT_EQ(run({"SMEMBERS", "s"}), array({bulk("a"), bulk("b")}));
  EXPECT_EQ(run({"ZRANGE", "z", "0", "-1"}), array({bulk("one"), bulk("two"), bulk("three")}));
  EXPECT_EQ(run({"LRANGE", "l", "0", "-1"}), array({bulk("w"), bulk("x"), bulk("y")}));
  EXPECT_EQ(run({"TYPE", "z"}), "+zset\r\n");
  EXPECT_EQ(run({"GET", "h"}).substr(0, 10), "-WRONGTYPE");
}

TEST_F(CollectionsTest, DelDropsElementsAndNewVersionStartsEmpty) {
  for (int i = 0; i < 100; ++i) run({"HSET", "h", "f" + std::to_string(i), "v"});
  EXPECT_EQ(run({"HLEN", "h"}), integer(100));
  uint64_t v1 = version_of("h");
  EXPECT_EQ(run({"DEL", "h"}), integer(1));
  EXPECT_EQ(data_entries(), 0u);  // range tombstone
  EXPECT_EQ(run({"HSET", "h", "new", "1"}), integer(1));
  EXPECT_GT(version_of("h"), v1);
  EXPECT_EQ(run({"HGETALL", "h"}), array({bulk("new"), bulk("1")}));
}

TEST_F(CollectionsTest, VersionsAreNeverReusedAfterRestart) {
  run({"SADD", "s", "a"});
  uint64_t v1 = version_of("s");
  close();
  open();
  run({"SADD", "t", "a"});
  EXPECT_GT(version_of("t"), v1);
}

TEST_F(CollectionsTest, CompactionReclaimsOrphanedElements) {
  for (int i = 0; i < 50; ++i) run({"RPUSH", "l", std::to_string(i)});
  run({"SADD", "keep", "x"});
  run({"SET", "l", "now a string"});  // blind write: the list elements are orphaned
  EXPECT_EQ(data_entries(), 51u);
  storage_->flush();
  storage_->compact();
  EXPECT_EQ(data_entries(), 1u);
  EXPECT_EQ(run({"SMEMBERS", "keep"}), array({bulk("x")}));
}

TEST_F(CollectionsTest, ExpiredCollectionDisappearsAndIsCompacted) {
  run({"ZADD", "z", "1", "a", "2", "b"});
  run({"PEXPIRE", "z", "30"});
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  storage_->flush();
  storage_->compact();  // drops the expired meta record, then its elements
  EXPECT_EQ(data_entries(), 0u);
  EXPECT_EQ(run({"ZCARD", "z"}), integer(0));
  EXPECT_EQ(run({"EXISTS", "z"}), integer(0));
}

TEST_F(CollectionsTest, RenameMovesElements) {
  run({"ZADD", "a", "1", "x", "2", "y"});
  run({"SADD", "b", "old"});
  EXPECT_EQ(run({"RENAME", "a", "b"}), "+OK\r\n");
  EXPECT_EQ(run({"ZRANGE", "b", "0", "-1", "WITHSCORES"}), array({bulk("x"), bulk("1"), bulk("y"), bulk("2")}));
  EXPECT_EQ(run({"EXISTS", "a"}), integer(0));
  EXPECT_EQ(data_entries(), 4u);  // two entries per zset member; a's and the old set's are gone
}

TEST_F(CollectionsTest, PipelineBatchSeesItsOwnElements) {
  {
    Database::WriteBatchScope batch(*db_);
    EXPECT_EQ(run({"RPUSH", "l", "a", "b", "c"}), integer(3));
    EXPECT_EQ(run({"LRANGE", "l", "0", "-1"}), array({bulk("a"), bulk("b"), bulk("c")}));
    EXPECT_EQ(run({"LREM", "l", "0", "b"}), integer(1));
    EXPECT_EQ(run({"ZADD", "z", "5", "m"}), integer(1));
    EXPECT_EQ(run({"ZINCRBY", "z", "1", "m"}), bulk("6"));
    EXPECT_EQ(run({"ZRANGEBYSCORE", "z", "6", "6"}), array({bulk("m")}));
    EXPECT_EQ(run({"SADD", "s1", "a", "b"}), integer(2));
    EXPECT_EQ(run({"SADD", "s2", "b", "c"}), integer(2));
    EXPECT_EQ(run({"SINTERSTORE", "s3", "s1", "s2"}), integer(1));  // multi-key: commits first
    EXPECT_EQ(run({"HSET", "h", "f", "1"}), integer(1));
    EXPECT_EQ(run({"DEL", "h"}), integer(1));
    EXPECT_EQ(run({"HGET", "h", "f"}), "$-1\r\n");
    batch.commit();
  }
  close();
  open();
  EXPECT_EQ(run({"LRANGE", "l", "0", "-1"}), array({bulk("a"), bulk("c")}));
  EXPECT_EQ(run({"SMEMBERS", "s3"}), array({bulk("b")}));
  EXPECT_EQ(run({"EXISTS", "h"}), integer(0));
}

TEST_F(CollectionsTest, FailedTransactionLeavesNoTrace) {
  run({"HSET", "h", "a", "1"});
  EXPECT_THROW(db_->transact("h",
                             [](Database::KeyTxn& t) {
                               t.put("b", "2");
                               ++t.meta().size;
                               if (t.size() > 0) throw std::runtime_error("boom");
                             }),
               std::runtime_error);
  EXPECT_EQ(run({"HGETALL", "h"}), array({bulk("a"), bulk("1")}));
  EXPECT_EQ(run({"HLEN", "h"}), integer(1));
}

TEST_F(CollectionsTest, ZsetOrdersNegativeAndFractionalScores) {
  run({"ZADD", "z", "-1.5", "a", "0", "b", "-inf", "c", "+inf", "d", "2.25", "e", "-0", "f"});
  EXPECT_EQ(run({"ZRANGE", "z", "0", "-1"}),
            array({bulk("c"), bulk("a"), bulk("b"), bulk("f"), bulk("e"), bulk("d")}));
  EXPECT_EQ(run({"ZRANGEBYSCORE", "z", "(-1.5", "2.25"}), array({bulk("b"), bulk("f"), bulk("e")}));
  EXPECT_EQ(run({"ZRANK", "z", "e"}), integer(4));
  EXPECT_EQ(run({"ZREVRANK", "z", "e"}), integer(1));
  EXPECT_EQ(run({"ZSCORE", "z", "c"}), bulk("-inf"));
}

TEST_F(CollectionsTest, ConcurrentListPushesKeepEveryElement) {
  constexpr int kThreads = 4, kPushes = 500;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      ClientState client;
      for (int i = 0; i < kPushes; ++i) {
        std::string out;
        Reply reply(out, 2);
        std::string v = std::to_string(t);
        std::vector<std::string_view> args{(i % 2) ? "LPUSH" : "RPUSH", "l", v};
        Database::WriteBatchScope batch(*db_);
        execute_redis_command(*ctx_, client, args, reply);
        batch.commit();
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(run({"LLEN", "l"}), integer(kThreads * kPushes));
  close();
  open();
  std::string all = run({"LRANGE", "l", "0", "-1"});
  EXPECT_EQ(all.substr(0, all.find('\r')), "*" + std::to_string(kThreads * kPushes));
}

TEST_F(CollectionsTest, CachedSmallCollectionsMatchRocksDB) {
  std::mt19937 rng(7);
  auto pick = [&](int n) { return std::to_string(rng() % n); };
  // Reads every collection fully, from the cache and then from RocksDB only.
  auto snapshot = [&] {
    return run({"HGETALL", "h"}) + run({"SMEMBERS", "s"}) + run({"ZRANGE", "z", "0", "-1", "WITHSCORES"}) +
           run({"LRANGE", "l", "0", "-1"}) + run({"HLEN", "h"}) + run({"ZCARD", "z"}) + run({"LLEN", "l"});
  };
  for (int round = 0; round < 40; ++round) {
    Database::WriteBatchScope batch(*db_, round % 2 == 0);  // with and without a pipeline batch
    for (int i = 0; i < 50; ++i) {
      switch (rng() % 12) {
        case 0: run({"HSET", "h", "f" + pick(40), pick(1000)}); break;
        case 1: run({"HDEL", "h", "f" + pick(40)}); break;
        case 2: run({"HINCRBY", "h", "n", "1"}); break;
        case 3: run({"SADD", "s", "m" + pick(40)}); break;
        case 4: run({"SREM", "s", "m" + pick(40)}); break;
        case 5: run({"ZADD", "z", pick(10), "m" + pick(40)}); break;
        case 6: run({"ZREM", "z", "m" + pick(40)}); break;
        case 7: run({"RPUSH", "l", pick(100)}); break;
        case 8: run({"LPOP", "l"}); break;
        case 9: run({"LREM", "l", "1", pick(100)}); break;
        case 10: run({"LINSERT", "l", "BEFORE", pick(100), "x"}); break;
        case 11:
          if (rng() % 10 == 0) run({"DEL", "h", "s", "z", "l"});
          break;
      }
    }
    batch.commit();
    std::string cached = snapshot();
    cache_->clear();
    EXPECT_EQ(snapshot(), cached) << "round " << round;
  }
}

TEST_F(CollectionsTest, LargeCollectionsAreNotCachedWhole) {
  for (int i = 0; i < 200; ++i) run({"SADD", "big", std::to_string(i)});  // more than kMaxCachedElements
  std::string big(3000, 'v');
  run({"HSET", "wide", "a", big, "b", big});  // fits the element limit, not the object size
  EXPECT_FALSE(db_->peek("big", true, [](Database::KeyTxn&) {}));
  EXPECT_FALSE(db_->peek("wide", true, [](Database::KeyTxn&) {}));
  EXPECT_TRUE(db_->peek("big", false, [](Database::KeyTxn& t) { EXPECT_EQ(t.size(), 200u); }));
  EXPECT_EQ(run({"SCARD", "big"}), integer(200));
  EXPECT_EQ(run({"HSTRLEN", "wide", "b"}), integer(3000));
  for (int i = 0; i < 150; ++i) run({"SREM", "big", std::to_string(i)});  // small again
  run({"SISMEMBER", "big", "199"});  // reads it whole once more
  EXPECT_TRUE(db_->peek("big", true, [](Database::KeyTxn& t) { EXPECT_TRUE(t.in_memory()); }));
  EXPECT_EQ(run({"SCARD", "big"}), integer(50));
}
