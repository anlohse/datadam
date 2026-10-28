// The change feed: CDC.INFO / CDC.READ over the retained WAL.
#include <gtest/gtest.h>

#include <cstdint>
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

// A RESP2 reply: a bulk/simple/integer/error string, or an array of replies.
struct Value {
  std::string text;
  bool array = false, error = false;
  std::vector<Value> items;
};

Value parse(std::string_view& in) {
  Value v;
  char type = in[0];
  size_t eol = in.find("\r\n");
  std::string_view head = in.substr(1, eol - 1);
  in.remove_prefix(eol + 2);
  if (type == '*') {
    v.array = true;
    int n = std::stoi(std::string(head));
    for (int i = 0; i < n; ++i) v.items.push_back(parse(in));
  } else if (type == '$') {
    int n = std::stoi(std::string(head));
    if (n >= 0) {
      v.text = std::string(in.substr(0, static_cast<size_t>(n)));
      in.remove_prefix(static_cast<size_t>(n) + 2);
    }
  } else {
    v.text = std::string(head);
    v.error = type == '-';
  }
  return v;
}

class CdcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_cdc_" + std::to_string(std::random_device{}()));
    open(3600);
  }
  void TearDown() override {
    close();
    fs::remove_all(dir_);
  }

  void open(uint64_t retention_s, uint64_t retention_bytes = 0, uint64_t hold_bytes = 0) {
    StorageOptions so;
    so.dir = dir_.string();
    so.fsync = FsyncPolicy::No;
    so.wal_retention_s = retention_s;
    so.wal_retention_bytes = retention_bytes;
    so.wal_hold_bytes = hold_bytes;
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

  Value run(std::vector<std::string> args, ClientState& client) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, 2);
    execute_redis_command(*ctx_, client, views, reply);
    if (out.empty()) return {};
    std::string_view in = out;
    return parse(in);
  }
  Value run(std::vector<std::string> args) { return run(std::move(args), client_); }

  // CDC.READ as "op key args..." lines (meta lines without their version), and the next sequence.
  std::vector<std::string> changes(std::vector<std::string> read, uint64_t* next = nullptr,
                                   ClientState* client = nullptr) {
    Value v = run(std::move(read), client ? *client : client_);
    EXPECT_TRUE(v.array) << v.text;
    std::vector<std::string> out;
    if (!v.array) return out;
    if (next) *next = std::stoull(v.items[0].text);
    for (const Value& batch : v.items[1].items) {
      EXPECT_GT(std::stoll(batch.items[1].text), 0) << "commit time";
      for (const Value& c : batch.items[2].items) {
        std::string line;
        for (size_t i = 0; i < c.items.size(); ++i) {
          if (c.items[0].text == "meta" && i == 5) continue;  // the version
          line += (line.empty() ? "" : " ") + c.items[i].text;
        }
        out.push_back(line);
      }
    }
    return out;
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

}  // namespace

TEST_F(CdcTest, DecodesStringsAndCollections) {
  run({"SET", "s", "v"});
  run({"HSET", "h", "f1", "v1", "f2", "v2"});
  run({"HDEL", "h", "f1"});
  run({"SADD", "st", "m1"});
  run({"SREM", "st", "m1"});  // the set empties: only the key's removal shows
  run({"ZADD", "z", "1.5", "a"});
  run({"ZREM", "z", "a"});
  run({"RPUSH", "l", "x", "y"});
  run({"LPUSH", "l", "w"});
  run({"DEL", "s"});
  run({"RENAME", "h", "h2"});
  run({"MSET", "a", "1", "b", "2"});
  std::vector<std::string> expected = {
      "set s v 0",
      "meta h hash 2 0", "hset h f1 v1", "hset h f2 v2",
      "meta h hash 1 0", "hdel h f1",
      "meta st set 1 0", "sadd st m1",
      "del st",
      "meta z zset 1 0", "zadd z 1.5 a",
      "del z",
      "meta l list 2 0 0", "lset l 0 x", "lset l 1 y",
      "meta l list 3 0 -1", "lset l -1 w",
      "del s",
      "meta h2 hash 1 0", "del h", "hset h2 f2 v2",
      "set a 1 0", "set b 2 0",
  };
  EXPECT_EQ(changes({"CDC.READ", "0", "COUNT", "1000"}), expected);
}

TEST_F(CdcTest, CursorCountMatchAndNewChangesOnly) {
  uint64_t next = 0;
  EXPECT_TRUE(changes({"CDC.READ", "$"}, &next).empty());
  run({"SET", "k1", "a"});
  run({"HSET", "h", "f", "v"});
  run({"SET", "k2", "b"});
  // Whole batches until COUNT is reached, then continue from `next`.
  EXPECT_EQ(changes({"CDC.READ", std::to_string(next), "COUNT", "1"}, &next), std::vector<std::string>{"set k1 a 0"});
  EXPECT_EQ(changes({"CDC.READ", std::to_string(next), "COUNT", "1"}, &next),
            (std::vector<std::string>{"meta h hash 1 0", "hset h f v"}));
  EXPECT_EQ(changes({"CDC.READ", std::to_string(next)}, &next), std::vector<std::string>{"set k2 b 0"});
  EXPECT_TRUE(changes({"CDC.READ", std::to_string(next)}, &next).empty());
  EXPECT_EQ(changes({"CDC.READ", "0", "MATCH", "k*"}), (std::vector<std::string>{"set k1 a 0", "set k2 b 0"}));
  // EXPIRE writes the record again with its TTL.
  run({"EXPIRE", "k1", "100"});
  auto c = changes({"CDC.READ", std::to_string(next)}, &next);
  ASSERT_EQ(c.size(), 1u);
  EXPECT_TRUE(c[0].starts_with("set k1 a ")) << c[0];
  EXPECT_NE(c[0], "set k1 a 0");
  run({"FLUSHALL"});
  c = changes({"CDC.READ", std::to_string(next), "COUNT", "1000"});
  EXPECT_EQ(c, std::vector<std::string>{"flushall "});  // op and an empty key
}

TEST_F(CdcTest, OnlyTheKeysTheUserMayRead) {
  run({"SET", "pub:1", "a"});
  run({"SET", "secret", "b"});
  run({"ACL", "SETUSER", "reader", "on", "nopass", "~pub:*", "+cdc.read"});
  ClientState reader;
  reader.user = ctx_->acl.find("reader");
  EXPECT_EQ(changes({"CDC.READ", "0"}, nullptr, &reader), std::vector<std::string>{"set pub:1 a 0"});
}

TEST_F(CdcTest, BlockWaitsForTheNextWrite) {
  uint64_t next = 0;
  changes({"CDC.READ", "$"}, &next);
  // Without a connection that can wait, BLOCK just returns nothing.
  EXPECT_TRUE(changes({"CDC.READ", std::to_string(next), "BLOCK", "100"}).empty());
  ClientState waiter;
  waiter.can_block = true;
  Value v = run({"CDC.READ", "$", "BLOCK", "0"}, waiter);
  EXPECT_FALSE(v.array);  // no reply yet
  EXPECT_TRUE(waiter.retry_later);
  EXPECT_TRUE(waiter.wait_for_writes);
  bool woken = false;
  uint64_t id = storage_->on_next_write(waiter.wait_after_seq, [&] { woken = true; });
  ASSERT_NE(id, 0u);
  run({"SET", "k", "v"});
  EXPECT_TRUE(woken);
  EXPECT_EQ(storage_->on_next_write(waiter.wait_after_seq, [] {}), 0u);  // already past
  // Run again (as the connection does): it resumes where it blocked, even with $.
  waiter.retry_later = waiter.wait_for_writes = false;
  EXPECT_EQ(changes({"CDC.READ", "$", "BLOCK", "0"}, nullptr, &waiter), std::vector<std::string>{"set k v 0"});
  EXPECT_FALSE(waiter.retry_later);
  EXPECT_EQ(waiter.block_deadline_ms, 0);
}

TEST_F(CdcTest, InfoAndErrors) {
  Value info = run({"CDC.INFO"});
  ASSERT_TRUE(info.array);
  EXPECT_EQ(info.items[1].text, "yes");
  EXPECT_TRUE(run({"CDC.READ", "abc"}).error);
  EXPECT_TRUE(run({"CDC.READ", "0", "COUNT", "0"}).error);
  EXPECT_TRUE(run({"CDC.READ", "0", "BOGUS", "1"}).error);
  close();
  open(0);  // no retention: the feed is off
  info = run({"CDC.INFO"});
  EXPECT_EQ(info.items[1].text, "no");
  Value v = run({"CDC.READ", "0"});
  EXPECT_TRUE(v.error);
  EXPECT_NE(v.text.find("wal-retention"), std::string::npos);
}

TEST_F(CdcTest, ChangesSurviveARestart) {
  run({"SET", "k", "v"});
  close();
  open(3600);
  run({"SET", "k2", "v2"});
  EXPECT_EQ(changes({"CDC.READ", "0"}), (std::vector<std::string>{"set k v 0", "set k2 v2 0"}));
}

TEST_F(CdcTest, KeptWalBeyondTheSizeLimitIsDropped) {
  close();
  open(3600, 1);  // keep at most one byte: any archived WAL file goes
  run({"SET", "a", "1"});
  uint64_t first = storage_->latest_sequence();
  storage_->flush();  // a new WAL file; the old one is archived
  run({"SET", "b", "2"});
  storage_->flush();
  size_t deleted = 0;
  for (int i = 0; i < 200 && deleted == 0; ++i) {  // archiving happens in the background
    deleted = storage_->trim_wal();
    if (deleted == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(deleted, 0u);
  Value v = run({"CDC.READ", std::to_string(first)});
  EXPECT_TRUE(v.error);
  EXPECT_NE(v.text.find("no longer"), std::string::npos) << v.text;
  run({"SET", "c", "3"});
  auto c = changes({"CDC.READ", "0"});  // from the oldest still kept
  ASSERT_FALSE(c.empty());
  EXPECT_EQ(c.back(), "set c 3 0");
}

TEST_F(CdcTest, WalReaderGoesOnFromWhereItStopped) {
  run({"SET", "a", "1"});
  uint64_t from = storage_->latest_sequence() + 1;
  RocksStorage::WalReader wal(*storage_, from);
  std::vector<RocksStorage::RawBatch> got;
  EXPECT_EQ(wal.read(1 << 20, &got), 0u);  // nothing written since
  run({"SET", "b", "2"});
  run({"SET", "c", "3"});
  EXPECT_EQ(wal.read(1, &got), 1u);  // about one byte: a single batch
  wal.read(1 << 20, &got);
  run({"SET", "d", "4"});  // at the tail of the WAL file it reads
  EXPECT_GT(wal.read(1 << 20, &got), 0u);
  storage_->flush();  // RocksDB goes on to a new WAL file
  run({"SET", "e", "5"});
  EXPECT_GT(wal.read(1 << 20, &got), 0u);
  EXPECT_EQ(wal.read(1 << 20, &got), 0u);
  // The same batches a fresh read finds.
  std::vector<RocksStorage::RawBatch> all;
  storage_->read_wal_raw(from, SIZE_MAX, &all);
  ASSERT_GE(all.size(), 4u);
  ASSERT_EQ(got.size(), all.size());
  for (size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(got[i].seq, all[i].seq);
    EXPECT_EQ(got[i].data, all[i].data);
  }
  EXPECT_EQ(got.back().seq + got.back().count, storage_->latest_sequence() + 1);
}

TEST_F(CdcTest, WalAReplicaNeedsIsHeld) {
  close();
  open(3600, 1, 1ULL << 30);  // keep one byte, but up to 1 GB more that a replica needs
  run({"SET", "a", "1"});
  uint64_t first = storage_->latest_sequence();
  storage_->flush();  // a new WAL file; the old one is archived
  run({"SET", "b", "2"});
  storage_->flush();
  storage_->hold_wal(first);
  for (int i = 0; i < 50; ++i) {  // archiving happens in the background
    EXPECT_EQ(storage_->trim_wal(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_FALSE(run({"CDC.READ", std::to_string(first)}).error);
  // Once no replica needs it, the size bound applies again.
  storage_->hold_wal(0);
  size_t deleted = 0;
  for (int i = 0; i < 200 && deleted == 0; ++i) {
    deleted = storage_->trim_wal();
    if (deleted == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(deleted, 0u);
  EXPECT_TRUE(run({"CDC.READ", std::to_string(first)}).error);
}

TEST_F(CdcTest, JsonDocuments) {
  uint64_t next = 0;
  changes({"CDC.READ", "$"}, &next);
  run({"JSON.SET", "doc", "$", R"({"a":1})"});
  run({"JSON.NUMINCRBY", "doc", "$.a", "1"});
  EXPECT_EQ(changes({"CDC.READ", std::to_string(next)}), (std::vector<std::string>{R"(json doc {"a":1} 0)",
                                                                                    R"(json doc {"a":2} 0)"}));
}

TEST(WalBoundTest, ASeldomWrittenColumnFamilyDoesNotHoldTheWal) {
  fs::path dir = fs::temp_directory_path() / ("datadam_walbound_" + std::to_string(std::random_device{}()));
  StorageOptions so;
  so.dir = dir.string();
  so.fsync = FsyncPolicy::No;
  so.write_buffer = 256 << 10;  // the live WAL is bounded at 2 MB
  {
    auto st = RocksStorage::open(so);
    st->put(Cf::System, "seldom", "written");  // this column family's only write
    std::string value(1000, 'x');
    for (int i = 0; i < 20000; ++i) st->put("k" + std::to_string(i), value);  // 20 MB in another
    // Left to RocksDB, the bound would be 4 times all the memtables, 20 MB: every WAL file would stay.
    bool bounded = false;
    for (int i = 0; i < 500 && !bounded; ++i) {  // flushes run in the background
      bounded = st->live_wal_bytes() < (8u << 20);
      if (!bounded) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(bounded) << st->live_wal_bytes();
  }
  fs::remove_all(dir);
}
