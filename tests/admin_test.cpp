// SCAN family, CONFIG SET and AUTH, through execute_redis_command.

#include <gtest/gtest.h>

#include <filesystem>
#include <future>
#include <thread>
#include <random>
#include <set>

#include "cache/object_cache.h"
#include "db/database.h"
#include "server/memcache_protocol.h"
#include "server/read_pool.h"
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

class AdminTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_admin_" + std::to_string(std::random_device{}()));
    StorageOptions so;
    so.dir = dir_.string();
    so.fsync = FsyncPolicy::No;
    storage_ = RocksStorage::open(so);
    CacheOptions co;
    co.max_memory = 4 << 20;
    co.min_memory = 0;
    co.shards = 2;
    cache_ = std::make_unique<ObjectCache>(co);
    db_ = std::make_unique<Database>(*storage_, *cache_);
    ctx_ = std::make_unique<ServerContext>(*db_, config_, stats_, 0);
  }
  void TearDown() override {
    ctx_.reset();
    db_.reset();
    cache_.reset();
    storage_.reset();
    fs::remove_all(dir_);
  }

  std::string raw(std::vector<std::string> args, ClientState& client) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, client.proto);
    execute_redis_command(*ctx_, client, views, reply);
    return out;
  }
  std::string raw(std::vector<std::string> args) { return raw(std::move(args), client_); }
  Value run(std::vector<std::string> args) {
    std::string out = raw(std::move(args));
    std::string_view in = out;
    return parse(in);
  }

  // Iterates a SCAN-family command to the end; returns every element (and
  // value, for HSCAN/ZSCAN) in order of arrival.
  std::vector<std::string> scan_all(std::vector<std::string> head, std::vector<std::string> tail, int* calls = nullptr) {
    std::vector<std::string> out;
    std::string cursor = "0";
    int n = 0;
    do {
      std::vector<std::string> args = head;
      args.push_back(cursor);
      args.insert(args.end(), tail.begin(), tail.end());
      Value v = run(args);
      EXPECT_TRUE(v.array) << v.text;
      if (!v.array) break;
      cursor = v.items[0].text;
      for (const auto& item : v.items[1].items) out.push_back(item.text);
      ++n;
    } while (cursor != "0" && n < 10000);
    if (calls) *calls = n;
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

TEST_F(AdminTest, ScanVisitsEveryKeyExactlyOnce) {
  std::set<std::string> all, users;
  for (int i = 0; i < 1000; ++i) {
    std::string k = (i % 3 ? "user:" : "item:") + std::to_string(i);
    if (i % 5 == 0) run({"HSET", k, "f", "v"});
    else run({"SET", k, "v"});
    all.insert(k);
    if (k.starts_with("user:")) users.insert(k);
  }
  int calls = 0;
  auto keys = scan_all({"SCAN"}, {"COUNT", "37"}, &calls);
  EXPECT_EQ(keys.size(), all.size());  // no duplicates...
  EXPECT_EQ(std::set<std::string>(keys.begin(), keys.end()), all);  // ...and nothing missing
  EXPECT_GE(calls, 1000 / 37);
  auto matched = scan_all({"SCAN"}, {"MATCH", "user:*", "COUNT", "50"});
  EXPECT_EQ(std::set<std::string>(matched.begin(), matched.end()), users);
  auto hashes = scan_all({"SCAN"}, {"MATCH", "user:*", "TYPE", "hash", "COUNT", "1000"});
  for (const auto& k : hashes) EXPECT_EQ(run({"TYPE", k}).text, "hash");
  EXPECT_FALSE(hashes.empty());
}

TEST_F(AdminTest, ScanReturnsKeysPresentThroughoutDespiteWrites) {
  for (int i = 0; i < 300; ++i) run({"SET", "stable:" + std::to_string(i), "v"});
  std::set<std::string> seen;
  std::string cursor = "0";
  int round = 0;
  do {
    Value v = run({"SCAN", cursor, "COUNT", "20"});
    cursor = v.items[0].text;
    for (const auto& item : v.items[1].items) {
      EXPECT_TRUE(seen.insert(item.text).second) << "returned twice: " << item.text;
    }
    // Churn between calls: keys come and go around the stable ones.
    run({"SET", "churn:" + std::to_string(round), "v"});
    run({"DEL", "churn:" + std::to_string(round - 1)});
    ++round;
  } while (cursor != "0");
  for (int i = 0; i < 300; ++i) EXPECT_TRUE(seen.contains("stable:" + std::to_string(i)));
}

TEST_F(AdminTest, CursorsAreReusableAndValidated) {
  for (int i = 0; i < 30; ++i) run({"SET", "k" + std::to_string(i), "v"});
  Value first = run({"SCAN", "0", "COUNT", "10"});
  std::string cursor = first.items[0].text;
  ASSERT_NE(cursor, "0");
  Value a = run({"SCAN", cursor, "COUNT", "10"});
  Value b = run({"SCAN", cursor, "COUNT", "10"});  // the same cursor again: the same continuation
  EXPECT_EQ(a.items[1].items.size(), b.items[1].items.size());
  EXPECT_EQ(a.items[1].items[0].text, b.items[1].items[0].text);
  EXPECT_TRUE(run({"SCAN", "12345"}).error);
  EXPECT_TRUE(run({"SCAN", "abc"}).error);
  EXPECT_TRUE(run({"HSCAN", "h", cursor}).error);  // a SCAN cursor is not an HSCAN cursor
  EXPECT_TRUE(run({"SCAN", "0", "COUNT", "0"}).error);
}

TEST_F(AdminTest, CollectionScans) {
  for (int size : {5, 300}) {  // cached whole, and not
    std::string n = std::to_string(size);
    std::set<std::string> fields, members;
    for (int i = 0; i < size; ++i) {
      std::string e = "e" + std::to_string(i);
      run({"HSET", "h" + n, e, "v" + std::to_string(i)});
      run({"SADD", "s" + n, e});
      run({"ZADD", "z" + n, std::to_string(i) + ".5", e});
      fields.insert(e);
      members.insert(e);
    }
    auto h = scan_all({"HSCAN", "h" + n}, {"COUNT", "7"});
    ASSERT_EQ(h.size(), fields.size() * 2);
    for (size_t i = 0; i < h.size(); i += 2) EXPECT_EQ("v" + h[i].substr(1), h[i + 1]);
    auto hk = scan_all({"HSCAN", "h" + n}, {"COUNT", "7", "NOVALUES"});
    EXPECT_EQ(std::set<std::string>(hk.begin(), hk.end()), fields);
    auto s = scan_all({"SSCAN", "s" + n}, {"COUNT", "11"});
    EXPECT_EQ(s.size(), members.size());
    EXPECT_EQ(std::set<std::string>(s.begin(), s.end()), members);
    auto z = scan_all({"ZSCAN", "z" + n}, {"COUNT", "13"});
    ASSERT_EQ(z.size(), members.size() * 2);
    for (size_t i = 0; i < z.size(); i += 2) EXPECT_EQ(z[i + 1], z[i].substr(1) + ".5");
    auto m = scan_all({"SSCAN", "s" + n}, {"MATCH", "e1*"});
    for (const auto& e : m) EXPECT_TRUE(e.starts_with("e1"));
    EXPECT_FALSE(m.empty());
  }
  EXPECT_EQ(raw({"HSCAN", "missing", "0"}), "*2\r\n$1\r\n0\r\n*0\r\n");
  run({"SET", "str", "v"});
  EXPECT_TRUE(run({"SSCAN", "str", "0"}).error);
}

TEST_F(AdminTest, ConfigSetAppliesAllOrNothing) {
  EXPECT_EQ(raw({"CONFIG", "SET", "cache-max-idle", "10s", "cache-on-write", "no", "fsync", "always"}), "+OK\r\n");
  Value got = run({"CONFIG", "GET", "cache-max-idle"});
  ASSERT_EQ(got.items.size(), 2u);
  EXPECT_EQ(got.items[1].text, "10s");
  EXPECT_EQ(cache_->options().max_idle_ms, 10'000);
  EXPECT_EQ(storage_->fsync_policy(), FsyncPolicy::Always);
  run({"SET", "k", "v"});
  EXPECT_FALSE(cache_->lookup("k", [](const RecordView&) {}));  // cache-on-write no

  // One bad pair: nothing at all changes.
  EXPECT_TRUE(run({"CONFIG", "SET", "cache-max-age", "1m", "cache-min-memory", "99gb"}).error);
  EXPECT_EQ(cache_->options().max_age_ms, 0);
  EXPECT_TRUE(run({"CONFIG", "SET", "port", "1"}).error);  // fixed at startup
  EXPECT_NE(raw({"CONFIG", "SET", "port", "1"}).find("immutable"), std::string::npos);
  EXPECT_NE(raw({"CONFIG", "SET", "nosuch", "1"}).find("Unknown option"), std::string::npos);
  EXPECT_NE(raw({"CONFIG", "SET", "fsync", "no", "fsync", "always"}).find("duplicate"), std::string::npos);
  EXPECT_TRUE(run({"CONFIG", "SET", "fsync", "sometimes"}).error);
  EXPECT_TRUE(run({"CONFIG", "SET", "fsync"}).error);
  EXPECT_EQ(raw({"CONFIG", "SET", "rocksdb-block-cache", "16mb", "rocksdb-write-buffer", "8mb"}), "+OK\r\n");
  EXPECT_EQ(raw({"CONFIG", "SET", "write-batching", "no", "log-level", "warn"}), "+OK\r\n");
  EXPECT_FALSE(ctx_->write_batching);
  EXPECT_NE(raw({"INFO", "persistence"}).find("fsync:always"), std::string::npos);
  EXPECT_EQ(raw({"CONFIG", "SET", "log-level", "info"}), "+OK\r\n");
}

TEST(ConfigTest, IoThreadsAutoFollowsTheCpus) {
  EXPECT_EQ(auto_io_threads(2), 2u);
  EXPECT_EQ(auto_io_threads(8), 8u);
  EXPECT_EQ(auto_io_threads(12), 8u);
  EXPECT_EQ(auto_io_threads(16), 8u);
  EXPECT_EQ(auto_io_threads(64), 32u);
  EXPECT_EQ(auto_io_threads(0), 4u);  // unknown
  Config c;
  EXPECT_EQ(c.io_thread_count(), auto_io_threads(std::thread::hardware_concurrency()));
  EXPECT_EQ(c.set("io-threads", "6"), "");
  EXPECT_EQ(c.io_thread_count(), 6u);
  EXPECT_NE(c.set("io-threads", "0"), "");
  EXPECT_EQ(c.set("io-threads", "AUTO"), "");
  EXPECT_EQ(c.io_threads, 0u);
  bool shown = false;
  for (const auto& [k, v] : c.dump()) shown |= k == "io-threads" && v == "auto";
  EXPECT_TRUE(shown);
}

TEST_F(AdminTest, AuthGatesEveryCommand) {
  EXPECT_NE(raw({"AUTH", "x"}).find("without any password configured"), std::string::npos);
  EXPECT_EQ(raw({"CONFIG", "SET", "requirepass", "s3cret"}), "+OK\r\n");
  EXPECT_EQ(raw({"GET", "k"}), "$-1\r\n");  // a connection already open stays authenticated

  ClientState fresh;
  fresh.authenticated = !ctx_->auth_required;
  EXPECT_EQ(raw({"GET", "k"}, fresh), "-NOAUTH Authentication required.\r\n");
  EXPECT_EQ(raw({"NOSUCHCMD"}, fresh), "-NOAUTH Authentication required.\r\n");
  EXPECT_EQ(raw({"HELLO", "3"}, fresh).substr(0, 7), "-NOAUTH");
  EXPECT_EQ(raw({"AUTH", "wrong"}, fresh).substr(0, 10), "-WRONGPASS");
  EXPECT_EQ(raw({"AUTH", "bob", "s3cret"}, fresh).substr(0, 10), "-WRONGPASS");
  EXPECT_EQ(raw({"AUTH", "s3cret"}, fresh), "+OK\r\n");
  EXPECT_EQ(raw({"GET", "k"}, fresh), "$-1\r\n");

  ClientState other;
  other.authenticated = false;
  EXPECT_EQ(raw({"HELLO", "2", "AUTH", "default", "nope"}, other).substr(0, 10), "-WRONGPASS");
  EXPECT_EQ(raw({"HELLO", "2", "AUTH", "default", "s3cret"}, other).substr(0, 4), "*14\r");
  EXPECT_TRUE(other.authenticated);
  EXPECT_NE(raw({"INFO", "stats"}).find("auth_failures:3"), std::string::npos);

  EXPECT_EQ(raw({"CONFIG", "SET", "requirepass", ""}), "+OK\r\n");
  EXPECT_FALSE(ctx_->auth_required);
}

TEST_F(AdminTest, MemcacheNeedsAuthWhenAPasswordIsSet) {
  raw({"CONFIG", "SET", "requirepass", "pw"});
  MemcacheSession session(*ctx_);
  auto send = [&](std::string_view req) {
    std::string out;
    size_t pos = 0;
    auto st = session.process(req, pos, out);
    return std::make_pair(st, out);
  };
  auto [st, out] = send("get k\r\n");
  EXPECT_EQ(st, MemcacheSession::Status::Close);
  EXPECT_NE(out.find("authentication required"), std::string::npos);
  EXPECT_EQ(send("auth bad\r\n").second, "CLIENT_ERROR authentication failed\r\n");
  EXPECT_EQ(send("auth pw\r\n").second, "OK\r\n");
  EXPECT_EQ(send("set k 0 0 1\r\nv\r\n").second, "STORED\r\n");
  EXPECT_EQ(send("get k\r\n").second, "VALUE k 0 1\r\nv\r\nEND\r\n");
}

TEST_F(AdminTest, ReadOfAnUncachedKeyIsDeferred) {
  run({"SET", "small", "s"});
  run({"SET", "cold", "c"});
  run({"SET", "big", std::string(10'000, 'b')});  // larger than the cache's objects
  storage_->flush();  // on disk, in a block not read yet: reading it is a file read
  cache_->erase("cold");
  ClientState reader;
  reader.defer_blocked_reads = true;
  uint64_t commands = stats_.total_commands.load();
  // The cached key is not answered alone: the whole command waits for "cold".
  EXPECT_EQ(raw({"MGET", "small", "cold"}, reader), "");
  EXPECT_TRUE(reader.retry_later);
  EXPECT_EQ(reader.load_key, "cold");
  EXPECT_EQ(db_->deferred_reads(), 1u);
  EXPECT_EQ(stats_.total_commands.load(), commands);  // counted when it really runs
  EXPECT_TRUE(db_->load_into_cache("cold"));
  reader.retry_later = false;
  std::string out = raw({"MGET", "small", "cold"}, reader);
  EXPECT_FALSE(reader.retry_later);
  EXPECT_EQ(out, "*2\r\n$1\r\ns\r\n$1\r\nc\r\n");
  // Keys that cannot be cached (too large, missing) are then read without deferring.
  EXPECT_FALSE(db_->load_into_cache("big"));
  EXPECT_FALSE(db_->load_into_cache("missing"));
  reader.defer_blocked_reads = false;
  EXPECT_EQ(raw({"EXISTS", "big", "missing"}, reader), ":1\r\n");
  // Commands that write are never deferred.
  cache_->erase("cold");
  reader.defer_blocked_reads = true;
  EXPECT_EQ(raw({"APPEND", "cold", "d"}, reader), ":2\r\n");
  EXPECT_FALSE(reader.retry_later);
  // What RocksDB answers from memory is read at once: a key in the memtable,
  // and a key that is not there at all.
  run({"SET", "fresh", "f"});
  cache_->erase("fresh");
  EXPECT_EQ(db_->memory_reads(), 0u);
  EXPECT_EQ(raw({"MGET", "fresh", "nosuchkey"}, reader), "*2\r\n$1\r\nf\r\n$-1\r\n");
  EXPECT_FALSE(reader.retry_later);
  EXPECT_EQ(db_->memory_reads(), 2u);
  EXPECT_EQ(db_->deferred_reads(), 1u);
}

TEST_F(AdminTest, ReadPoolLoadsAKeyOnceForAllWaiters) {
  run({"SET", "cold", "c"});
  cache_->erase("cold");
  // Another client writes a key with the same lock stripe, so the load waits
  // and the later requests find it still under way.
  std::string twin;
  for (int i = 0; twin.empty(); ++i) {
    std::string k = "twin" + std::to_string(i);
    if (((hash_bytes(k) ^ hash_bytes("cold")) & (KeyLocks::kStripes - 1)) == 0) twin = k;
  }
  std::promise<void> locked, release;
  std::thread holder([&] {
    Database::WriteBatchScope batch(*db_);
    Record r;
    r.value = "x";
    db_->put(twin, r);
    locked.set_value();
    release.get_future().wait();
    batch.commit();
  });
  locked.get_future().wait();
  ReadPool pool(*db_, 2);
  std::promise<bool> first, second;
  pool.load("cold", [&](bool cached) { first.set_value(cached); });
  pool.load("cold", [&](bool cached) { second.set_value(cached); });
  pool.load("cold", nullptr);  // a prefetch joins too
  release.set_value();
  holder.join();
  EXPECT_TRUE(first.get_future().get());
  EXPECT_TRUE(second.get_future().get());
  EXPECT_EQ(pool.loads(), 1u);
  EXPECT_EQ(pool.joined(), 2u);
  EXPECT_EQ(pool.latency().count(), 1u);
  auto v = db_->get("cold");
  ASSERT_TRUE(v);
  EXPECT_EQ(v->value, "c");
}

TEST_F(AdminTest, ReadPoolBatchesTheBacklog) {
  for (int i = 0; i < 9; ++i) run({"SET", "k" + std::to_string(i), std::to_string(i)});
  cache_->clear();
  // The only pool thread waits on k0 (its lock is held), so k1..k8 queue up
  // and are then read together, with one MultiGet.
  std::string twin;
  for (int i = 0; twin.empty(); ++i) {
    std::string k = "twin" + std::to_string(i);
    if (((hash_bytes(k) ^ hash_bytes("k0")) & (KeyLocks::kStripes - 1)) == 0) twin = k;
  }
  std::promise<void> locked, release;
  std::thread holder([&] {
    Database::WriteBatchScope batch(*db_);
    Record r;
    r.value = "x";
    db_->put(twin, r);
    locked.set_value();
    release.get_future().wait();
    batch.commit();
  });
  locked.get_future().wait();
  ReadPool pool(*db_, 1, 8);
  std::vector<std::promise<bool>> done(9);
  pool.load("k0", [&](bool cached) { done[0].set_value(cached); });
  while (pool.queued() > 0) std::this_thread::yield();  // k0 taken, alone
  for (int i = 1; i < 9; ++i) pool.load("k" + std::to_string(i), [&, i](bool cached) { done[i].set_value(cached); });
  uint64_t multigets = storage_->multi_get_latency().count();
  release.set_value();
  holder.join();
  for (auto& d : done) EXPECT_TRUE(d.get_future().get());
  EXPECT_EQ(pool.loads(), 9u);
  EXPECT_EQ(pool.batches(), 2u);
  EXPECT_EQ(storage_->multi_get_latency().count(), multigets + 1);
}
