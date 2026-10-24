#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <random>
#include <thread>

#include "cache/object_cache.h"
#include "db/database.h"
#include "server/redis_commands.h"
#include "storage/rocks_storage.h"

using namespace datadam;
namespace fs = std::filesystem;

namespace {

class IndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_idx_" + std::to_string(std::random_device{}()));
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

  std::string run(std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, 2);
    ClientState client;
    execute_redis_command(*ctx_, client, views, reply);
    return out;
  }

  // Waits for the backfill to finish; returns the final state.
  IndexState wait_built(const std::string& name) {
    for (int i = 0; i < 500; ++i) {
      auto index = db_->indexes().find(name);
      if (!index) return IndexState::Failed;
      if (index->state != IndexState::Building) return index->state;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return IndexState::Building;
  }

  size_t index_entries() {
    auto c = storage_->cursor(Cf::Index, "", "");
    size_t n = 0;
    for (c->seek_first(); c->valid(); c->next()) ++n;
    return n;
  }

  void users_index(bool unique_email = false) {
    ASSERT_EQ(run({"IDX.CREATE", "users", "PREFIX", "1", "user:", "SCHEMA", "city", "TAG", "age", "NUMERIC"}),
              "+OK\r\n");
    ASSERT_EQ(wait_built("users"), IndexState::Ready);
    if (unique_email) {
      ASSERT_EQ(run({"IDX.CREATE", "emails", "PREFIX", "1", "user:", "SCHEMA", "email", "TAG", "UNIQUE"}), "+OK\r\n");
      ASSERT_EQ(wait_built("emails"), IndexState::Ready);
    }
  }

  fs::path dir_;
  Config config_;
  ServerStats stats_;
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
std::string keys(std::initializer_list<std::string> ks) {
  std::string out = "*" + std::to_string(ks.size()) + "\r\n";
  for (const auto& k : ks) out += bulk(k);
  return out;
}

}  // namespace

TEST_F(IndexTest, QueriesUseEqualityAndRange) {
  users_index();
  run({"HSET", "user:1", "name", "ana", "city", "SP", "age", "30"});
  run({"HSET", "user:2", "name", "bia", "city", "SP", "age", "25"});
  run({"HSET", "user:3", "name", "caio", "city", "RJ", "age", "41"});
  run({"HSET", "user:4", "name", "duda", "city", "SP", "age", "-2.5"});
  run({"HSET", "user:5", "name", "eva", "city", "SP"});              // no age: not indexed
  run({"HSET", "other:1", "city", "SP", "age", "20"});               // outside the prefix
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "NOCONTENT"}), keys({"user:4", "user:2", "user:1"}));
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "age", ">", "0", "age", "<=", "30", "NOCONTENT"}),
            keys({"user:2", "user:1"}));
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "DESC", "LIMIT", "0", "1", "NOCONTENT"}), keys({"user:1"}));
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "LIMIT", "1", "5", "NOCONTENT"}),
            keys({"user:2", "user:1"}));
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "name", "=", "bia", "RETURN", "1", "age"}),
            array({bulk("user:2"), array({bulk("age"), bulk("25")})}));
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "RJ"}),
            array({bulk("user:3"), array({bulk("age"), bulk("41"), bulk("city"), bulk("RJ"), bulk("name"),
                                          bulk("caio")})}));
  EXPECT_EQ(run({"IDX.COUNT", "users", "city", "=", "SP"}), integer(3));
  EXPECT_EQ(run({"IDX.COUNT", "users"}), integer(4));  // whole index
  EXPECT_EQ(run({"IDX.COUNT", "users", "age", ">=", "30"}), integer(2));  // filter only
  EXPECT_EQ(run({"IDX.GET", "users", "SP", "25"}), array({bulk("user:2"), array({bulk("age"), bulk("25"), bulk("city"),
                                                                                bulk("SP"), bulk("name"),
                                                                                bulk("bia")})}));
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "age", ">", "x"}).substr(0, 4), "-ERR");
  EXPECT_EQ(run({"IDX.QUERY", "nope"}).substr(0, 4), "-ERR");
}

TEST_F(IndexTest, WritesKeepTheIndexCurrent) {
  users_index();
  run({"HSET", "user:1", "city", "SP", "age", "30"});
  run({"HSET", "user:2", "city", "SP", "age", "25"});
  run({"HSET", "user:1", "city", "RJ"});  // moves to another slice
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "NOCONTENT"}), keys({"user:2"}));
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "RJ", "NOCONTENT"}), keys({"user:1"}));
  run({"HINCRBY", "user:2", "age", "10"});
  EXPECT_EQ(run({"IDX.QUERY", "users", "age", ">=", "35", "NOCONTENT"}), keys({"user:2"}));
  run({"HDEL", "user:2", "age"});  // no longer indexed
  EXPECT_EQ(run({"IDX.COUNT", "users", "city", "=", "SP"}), integer(0));
  run({"RENAME", "user:1", "user:9"});
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "RJ", "NOCONTENT"}), keys({"user:9"}));
  run({"DEL", "user:9"});
  EXPECT_EQ(run({"IDX.COUNT", "users"}), integer(0));
  EXPECT_EQ(index_entries(), 0u);
  run({"HSET", "user:3", "city", "SP", "age", "1"});
  run({"PEXPIRE", "user:3", "20"});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(run({"IDX.COUNT", "users"}), integer(0));  // expired hashes are not returned
}

TEST_F(IndexTest, BackfillIndexesExistingHashesAndSurvivesRestart) {
  for (int i = 0; i < 1000; ++i) {
    run({"HSET", "item:" + std::to_string(i), "kind", i % 2 ? "odd" : "even", "n", std::to_string(i)});
  }
  run({"SET", "item:string", "not a hash"});
  ASSERT_EQ(run({"IDX.CREATE", "items", "PREFIX", "1", "item:", "SCHEMA", "kind", "TAG", "n", "NUMERIC"}), "+OK\r\n");
  ASSERT_EQ(wait_built("items"), IndexState::Ready);
  EXPECT_EQ(run({"IDX.COUNT", "items", "kind", "=", "odd"}), integer(500));
  EXPECT_EQ(run({"IDX.COUNT", "items", "kind", "=", "even", "n", "<", "100"}), integer(50));
  close();
  open();
  EXPECT_EQ(db_->indexes().find("items")->state, IndexState::Ready);
  EXPECT_EQ(run({"IDX.QUERY", "items", "kind", "=", "odd", "n", ">=", "997", "NOCONTENT"}),
            keys({"item:997", "item:999"}));
  EXPECT_EQ(run({"IDX.LIST"}), keys({"items"}));
}

TEST_F(IndexTest, UniqueIndexRejectsDuplicatesAndRollsBack) {
  users_index(true);
  EXPECT_EQ(run({"HSET", "user:1", "email", "a@x", "city", "SP"}), integer(2));
  std::string err = run({"HSET", "user:2", "email", "a@x", "city", "RJ"});
  EXPECT_EQ(err.substr(0, 26), "-ERR UNIQUE index 'emails'");
  EXPECT_EQ(run({"EXISTS", "user:2"}), integer(0));  // the whole command was undone
  EXPECT_EQ(run({"IDX.GET", "emails", "a@x"}), array({bulk("user:1"), array({bulk("city"), bulk("SP"), bulk("email"),
                                                                            bulk("a@x")})}));
  EXPECT_EQ(run({"HSET", "user:1", "email", "b@x"}), integer(0));  // changing its own value is fine
  EXPECT_EQ(run({"HSET", "user:2", "email", "a@x"}), integer(1));  // a@x is free now
  EXPECT_EQ(run({"IDX.GET", "emails", "nobody@x"}), "$-1\r\n");
  run({"SET", "user:1", "overwritten"});  // blind write leaves a stale entry
  EXPECT_EQ(run({"HSET", "user:3", "email", "b@x"}), integer(1));  // the stale owner does not block
}

TEST_F(IndexTest, UniqueBackfillFailsOnDuplicates) {
  run({"HSET", "u:1", "doc", "123"});
  run({"HSET", "u:2", "doc", "123"});
  ASSERT_EQ(run({"IDX.CREATE", "docs", "PREFIX", "1", "u:", "SCHEMA", "doc", "TAG", "UNIQUE"}), "+OK\r\n");
  EXPECT_EQ(wait_built("docs"), IndexState::Failed);
  EXPECT_NE(run({"IDX.INFO", "docs"}).find("UNIQUE"), std::string::npos);
  EXPECT_EQ(run({"IDX.QUERY", "docs"}).substr(0, 4), "-ERR");
  EXPECT_EQ(run({"IDX.DROP", "docs"}), "+OK\r\n");
  EXPECT_EQ(index_entries(), 0u);
}

TEST_F(IndexTest, DropRemovesEntries) {
  users_index();
  for (int i = 0; i < 20; ++i) run({"HSET", "user:" + std::to_string(i), "city", "SP", "age", "1"});
  EXPECT_EQ(index_entries(), 20u);
  EXPECT_EQ(run({"IDX.DROP", "users"}), "+OK\r\n");
  EXPECT_EQ(index_entries(), 0u);
  EXPECT_EQ(run({"IDX.QUERY", "users"}).substr(0, 4), "-ERR");
  run({"HSET", "user:1", "city", "RJ"});  // no index to maintain any more
  EXPECT_EQ(index_entries(), 0u);
}

TEST_F(IndexTest, StaleEntriesAreRepairedAndCompacted) {
  users_index();
  run({"HSET", "user:1", "city", "SP", "age", "1"});
  run({"HSET", "user:2", "city", "SP", "age", "2"});
  run({"SET", "user:1", "string now"});  // blind writes drop the entries too
  EXPECT_EQ(index_entries(), 1u);
  run({"MSET", "user:2", "also a string"});
  EXPECT_EQ(index_entries(), 0u);

  // Forged stale entries for a live hash: one with another tuple, one with an old version.
  run({"HSET", "user:3", "city", "SP", "age", "3"});
  const IndexDef& def = db_->indexes().find("users")->def;
  CollectionMeta m;
  ASSERT_TRUE(decode_collection_meta(db_->get("user:3")->value, &m));
  auto forge = [&](std::string_view age, uint64_t version) {
    std::string tuple;
    encode_index_value(IndexFieldType::Tag, "SP", &tuple);
    encode_index_value(IndexFieldType::Numeric, age, &tuple);
    auto b = storage_->batch();
    b.put(Cf::Index, def.entry_key(tuple, "user:3"), def.entry_value(version, 0, "user:3"));
    storage_->write(b);
  };
  forge("99", m.version);
  forge("98", m.version - 1);
  EXPECT_EQ(index_entries(), 3u);
  EXPECT_EQ(run({"IDX.QUERY", "users", "RETURN", "1", "age"}), array({bulk("user:3"), array({bulk("age"), bulk("3")})}));
  EXPECT_EQ(index_entries(), 1u);  // both were repaired by the query
  EXPECT_EQ(run({"IDX.COUNT", "users"}), integer(1));

  // The entry carries the TTL: an expired hash is not counted, and compaction drops its entry.
  run({"PEXPIRE", "user:3", "20"});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(run({"IDX.COUNT", "users"}), integer(0));
  storage_->flush();
  storage_->compact();
  EXPECT_EQ(index_entries(), 0u);
}

TEST_F(IndexTest, FullSyncKeepsEntriesUntilDefinitionsArrive) {
  users_index();
  run({"HSET", "user:1", "city", "SP", "age", "1"});
  // What a full sync sends, column family by column family.
  std::vector<std::tuple<Cf, std::string, std::string>> sent, system;
  for (Cf cf : {Cf::Meta, Cf::Data, Cf::Index, Cf::System}) {
    auto c = storage_->cursor(cf, "", std::string(64, '\xff'), false);
    for (c->seek_first(); c->valid(); c->next()) {
      (cf == Cf::System ? system : sent).emplace_back(cf, std::string(c->key()), std::string(c->value()));
    }
  }
  db_->clear_for_full_sync();
  auto apply = [&](const auto& rows) {
    auto b = storage_->batch();
    for (const auto& [cf, k, v] : rows) b.put(cf, k, v);
    storage_->write(b);
  };
  // The entries arrive before the definitions: compaction must not take them for a dropped index's.
  apply(sent);
  storage_->flush();
  storage_->compact();
  EXPECT_EQ(index_entries(), 1u);
  apply(system);
  db_->finish_full_sync();
  storage_->flush();
  storage_->compact();
  EXPECT_EQ(index_entries(), 1u);
  EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "NOCONTENT"}), array({bulk("user:1")}));
  // And the GC works again: entries of an index that does not exist go.
  {
    auto b = storage_->batch();
    b.put(Cf::Index, encode_index_id(999) + "orphan", std::string(16, '\0'));
    storage_->write(b);
  }
  EXPECT_EQ(index_entries(), 2u);
  storage_->flush();
  storage_->compact();
  EXPECT_EQ(index_entries(), 1u);
}

TEST_F(IndexTest, PipelinedWritesAreQueryable) {
  users_index(true);
  {
    Database::WriteBatchScope batch(*db_);
    EXPECT_EQ(run({"HSET", "user:1", "city", "SP", "age", "1", "email", "1@x"}), integer(3));
    EXPECT_EQ(run({"HSET", "user:2", "city", "SP", "age", "2", "email", "2@x"}), integer(3));
    EXPECT_EQ(run({"HSET", "user:3", "email", "1@x"}).substr(0, 4), "-ERR");
    EXPECT_EQ(run({"IDX.QUERY", "users", "city", "=", "SP", "NOCONTENT"}), keys({"user:1", "user:2"}));
    batch.commit();
  }
  EXPECT_EQ(run({"IDX.COUNT", "emails"}), integer(2));
}

TEST_F(IndexTest, ConcurrentUniqueClaimsLetOneWin) {
  users_index(true);
  constexpr int kThreads = 8;
  std::atomic<int> wins{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::string key = "user:" + std::to_string(t);
      std::vector<std::string> args{"HSET", key, "email", "same@x", "city", "SP", "age", std::to_string(t)};
      std::vector<std::string_view> views(args.begin(), args.end());
      std::string out;
      Reply reply(out, 2);
      ClientState client;
      Database::WriteBatchScope batch(*db_);
      execute_redis_command(*ctx_, client, views, reply);
      batch.commit();
      if (out == ":3\r\n") ++wins;
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(wins.load(), 1);
  EXPECT_EQ(run({"IDX.COUNT", "emails"}), integer(1));
  EXPECT_EQ(run({"IDX.COUNT", "users"}), integer(1));
}

TEST_F(IndexTest, ConcurrentWritersKeepCountsExact) {
  users_index();
  constexpr int kThreads = 4, kOps = 400, kKeys = 50;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(t);
      ClientState client;
      for (int i = 0; i < kOps; ++i) {
        std::string key = "user:" + std::to_string(rng() % kKeys);
        std::string city = rng() % 2 ? "SP" : "RJ";
        std::vector<std::string> args{"HSET", key, "city", city, "age", std::to_string(rng() % 90)};
        if (rng() % 10 == 0) args = {"DEL", key};
        std::vector<std::string_view> views(args.begin(), args.end());
        std::string out;
        Reply reply(out, 2);
        Database::WriteBatchScope batch(*db_);
        execute_redis_command(*ctx_, client, views, reply);
        batch.commit();
      }
    });
  }
  for (auto& th : threads) th.join();
  // Every hash is in the index exactly once, under its current values.
  int hashes = 0;
  for (int k = 0; k < kKeys; ++k) hashes += run({"EXISTS", "user:" + std::to_string(k)}) == integer(1);
  EXPECT_EQ(run({"IDX.COUNT", "users"}), integer(hashes));
  EXPECT_EQ(index_entries(), static_cast<size_t>(hashes));
}
