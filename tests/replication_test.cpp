// Replication between two real servers in this process.
#include <gtest/gtest.h>

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <thread>

#include "cache/object_cache.h"
#include "db/database.h"
#include "server/asio_util.h"
#include "server/redis_commands.h"
#include "server/server.h"
#include "storage/rocks_storage.h"

using namespace datadam;
using asio::ip::tcp;
namespace fs = std::filesystem;

namespace {

// A RESP2 reply: a bulk/simple/integer/error string, or an array of replies.
struct Value {
  std::string text;
  bool array = false, error = false;
  std::vector<Value> items;
};

// Parses one reply from the front of `in`; false if it is not complete yet.
bool parse(std::string_view& in, Value& v) {
  size_t eol = in.find("\r\n");
  if (eol == std::string_view::npos) return false;
  char type = in[0];
  std::string_view head = in.substr(1, eol - 1);
  std::string_view rest = in.substr(eol + 2);
  v = Value{};
  if (type == '*') {
    v.array = true;
    int n = std::stoi(std::string(head));
    for (int i = 0; i < n; ++i) {
      Value item;
      if (!parse(rest, item)) return false;
      v.items.push_back(std::move(item));
    }
  } else if (type == '$') {
    int n = std::stoi(std::string(head));
    if (n >= 0) {
      if (rest.size() < static_cast<size_t>(n) + 2) return false;
      v.text = std::string(rest.substr(0, static_cast<size_t>(n)));
      rest.remove_prefix(static_cast<size_t>(n) + 2);
    }
  } else {
    v.text = std::string(head);
    v.error = type == '-';
  }
  in = rest;
  return true;
}

// A blocking client connection.
class Client {
 public:
  explicit Client(uint16_t port) : socket_(io_) {
    socket_.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  }

  Value run(const std::vector<std::string>& args) {
    std::string out = std::format("*{}\r\n", args.size());
    for (const auto& a : args) out += std::format("${}\r\n{}\r\n", a.size(), a);
    asio::write(socket_, asio::buffer(out));
    for (;;) {
      std::string_view view = buf_;
      Value v;
      if (parse(view, v)) {
        buf_.erase(0, buf_.size() - view.size());
        return v;
      }
      char chunk[4096];
      size_t n = socket_.read_some(asio::buffer(chunk));
      buf_.append(chunk, n);
    }
  }

  std::string text(const std::vector<std::string>& args) { return run(args).text; }

 private:
  asio::io_context io_;
  tcp::socket socket_;
  std::string buf_;
};

// One server: storage, database and network, on ephemeral ports.
struct Node {
  Node(const fs::path& dir, uint64_t wal_retention_s, const std::string& replicaof = "",
       uint64_t wal_retention_bytes = 0) {
    StorageOptions so;
    so.dir = dir.string();
    so.fsync = FsyncPolicy::No;
    so.wal_retention_s = wal_retention_s;
    so.wal_retention_bytes = wal_retention_bytes;
    storage = RocksStorage::open(so);
    CacheOptions co;
    co.max_memory = 4 << 20;
    co.min_memory = 0;
    co.shards = 2;
    cache = std::make_unique<ObjectCache>(co);
    bool replica = !replicaof.empty();
    db = std::make_unique<Database>(*storage, *cache, true, !replica);
    db->set_read_only(replica);
    config.bind = "127.0.0.1";
    config.io_threads = 2;
    config.memcache_port = 0;
    config.read_threads = 4;
    config.replicaof = replicaof;
    ctx = std::make_unique<ServerContext>(*db, config, stats, 0);
    server = std::make_unique<Server>(*ctx);
    server->start(true);
    thread = std::thread([this] { server->run(); });
  }
  ~Node() {
    server->stop();
    thread.join();
    server.reset();
    ctx.reset();
    db.reset();
    cache.reset();
    storage.reset();
  }

  uint16_t port() const { return server->port(); }
  // A field of INFO replication.
  std::string info(const std::string& field) {
    std::string all = Client(port()).text({"INFO", "replication"});
    size_t at = all.find(field + ":");
    if (at == std::string::npos) return {};
    size_t end = all.find("\r\n", at);
    return all.substr(at + field.size() + 1, end - at - field.size() - 1);
  }

  Config config;
  ServerStats stats;
  std::unique_ptr<RocksStorage> storage;
  std::unique_ptr<ObjectCache> cache;
  std::unique_ptr<Database> db;
  std::unique_ptr<ServerContext> ctx;
  std::unique_ptr<Server> server;
  std::thread thread;
};

bool eventually(const std::function<bool()>& done, std::chrono::milliseconds limit = std::chrono::seconds(15)) {
  auto until = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < until) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return done();
}

class ReplicationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_repl_" + std::to_string(std::random_device{}()));
    primary_ = std::make_unique<Node>(dir_ / "primary", 3600);
  }
  void TearDown() override {
    replica_.reset();
    primary_.reset();
    fs::remove_all(dir_);
  }

  void start_replica() {
    replica_ = std::make_unique<Node>(dir_ / "replica", 3600, std::format("127.0.0.1 {}", primary_->port()));
  }
  // The replica has applied everything the primary wrote.
  bool caught_up() {
    return replica_->info("master_link_status") == "up" &&
           replica_->info("slave_repl_offset") == primary_->info("repl_offset");
  }

  fs::path dir_;
  std::unique_ptr<Node> primary_;
  std::unique_ptr<Node> replica_;
};

std::vector<std::string> texts(const Value& v) {
  std::vector<std::string> out;
  for (const auto& i : v.items) out.push_back(i.text);
  return out;
}

}  // namespace

TEST_F(ReplicationTest, FullSyncThenStream) {
  Client p(primary_->port());
  EXPECT_EQ(p.text({"SET", "s", "v"}), "OK");
  EXPECT_EQ(p.text({"HSET", "user:1", "city", "SP", "age", "30"}), "2");
  EXPECT_EQ(p.text({"SADD", "set", "a", "b"}), "2");
  EXPECT_EQ(p.text({"ZADD", "z", "1", "x", "2", "y"}), "2");
  EXPECT_EQ(p.text({"RPUSH", "l", "1", "2", "3"}), "3");
  EXPECT_EQ(p.text({"IDX.CREATE", "users", "PREFIX", "1", "user:", "SCHEMA", "city", "TAG"}), "OK");
  ASSERT_TRUE(eventually([&] { return p.run({"IDX.QUERY", "users", "city", "=", "SP"}).array; }));

  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); })) << replica_->info("master_link_status");
  EXPECT_EQ(replica_->info("full_syncs"), "1");
  Client r(replica_->port());
  EXPECT_EQ(r.text({"GET", "s"}), "v");
  EXPECT_EQ(texts(r.run({"HGETALL", "user:1"})), (std::vector<std::string>{"age", "30", "city", "SP"}));
  EXPECT_EQ(r.text({"SCARD", "set"}), "2");
  EXPECT_EQ(texts(r.run({"ZRANGE", "z", "0", "-1"})), (std::vector<std::string>{"x", "y"}));
  EXPECT_EQ(texts(r.run({"LRANGE", "l", "0", "-1"})), (std::vector<std::string>{"1", "2", "3"}));
  Value q = r.run({"IDX.QUERY", "users", "city", "=", "SP", "NOCONTENT"});
  ASSERT_TRUE(q.array) << q.text;
  EXPECT_EQ(texts(q), (std::vector<std::string>{"user:1"}));

  // Then every write streams over, and the cache follows.
  EXPECT_EQ(r.text({"GET", "s"}), "v");  // cached on the replica now
  EXPECT_EQ(p.text({"SET", "s", "v2"}), "OK");
  EXPECT_EQ(p.text({"HSET", "user:2", "city", "SP"}), "1");
  EXPECT_EQ(p.text({"DEL", "set"}), "1");
  EXPECT_EQ(p.text({"LPUSH", "l", "0"}), "4");
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_EQ(r.text({"GET", "s"}), "v2");
  EXPECT_EQ(r.text({"EXISTS", "set"}), "0");
  EXPECT_EQ(texts(r.run({"LRANGE", "l", "0", "-1"})), (std::vector<std::string>{"0", "1", "2", "3"}));
  EXPECT_EQ(r.text({"IDX.COUNT", "users", "city", "=", "SP"}), "2");

  // The replica refuses writes, and the roles show up.
  Value w = r.run({"SET", "s", "x"});
  EXPECT_TRUE(w.error);
  EXPECT_TRUE(w.text.starts_with("READONLY")) << w.text;
  EXPECT_EQ(primary_->info("connected_slaves"), "1");
  EXPECT_EQ(r.run({"ROLE"}).items[0].text, "slave");
  EXPECT_EQ(p.run({"ROLE"}).items[0].text, "master");

  // Dropped indexes and FLUSHALL follow too.
  EXPECT_EQ(p.text({"IDX.DROP", "users"}), "OK");
  EXPECT_EQ(p.text({"FLUSHALL"}), "OK");
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_TRUE(r.run({"IDX.QUERY", "users", "city", "=", "SP"}).error);
  EXPECT_TRUE(r.run({"KEYS", "*"}).items.empty());  // DBSIZE is an estimate
  EXPECT_EQ(r.run({"GET", "s"}).text, "");
}

TEST_F(ReplicationTest, RestartedReplicaContinues) {
  Client p(primary_->port());
  EXPECT_EQ(p.text({"SET", "a", "1"}), "OK");
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  replica_.reset();
  EXPECT_EQ(p.text({"SET", "b", "2"}), "OK");
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_EQ(replica_->info("full_syncs"), "0");
  EXPECT_EQ(replica_->info("partial_syncs"), "1");
  Client r(replica_->port());
  EXPECT_EQ(r.text({"GET", "a"}), "1");
  EXPECT_EQ(r.text({"GET", "b"}), "2");
}

TEST_F(ReplicationTest, PromotedReplicaTakesWrites) {
  Client p(primary_->port());
  EXPECT_EQ(p.text({"HSET", "h", "f", "v"}), "1");
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  Client r(replica_->port());
  EXPECT_EQ(r.text({"REPLICAOF", "NO", "ONE"}), "OK");
  EXPECT_EQ(r.run({"ROLE"}).items[0].text, "master");
  EXPECT_EQ(r.text({"SET", "new", "x"}), "OK");
  EXPECT_EQ(r.text({"HSET", "h2", "f", "v"}), "1");  // a collection version past the old primary's
  EXPECT_EQ(r.text({"HGET", "h", "f"}), "v");
  EXPECT_EQ(r.text({"HGET", "h2", "f"}), "v");
  // The old primary's writes no longer arrive.
  EXPECT_EQ(p.text({"SET", "late", "1"}), "OK");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(r.run({"GET", "late"}).text, "");
}

TEST_F(ReplicationTest, AfterAPromotionTheOthersContinue) {
  // A primary and two replicas; the second falls behind, then the first is promoted.
  Client p(primary_->port());
  EXPECT_EQ(p.text({"SET", "a", "1"}), "OK");
  start_replica();
  auto other = std::make_unique<Node>(dir_ / "other", 3600, std::format("127.0.0.1 {}", primary_->port()));
  auto other_caught_up = [&](Node& primary) {
    return other->info("master_link_status") == "up" &&
           other->info("slave_repl_offset") == primary.info("repl_offset");
  };
  ASSERT_TRUE(eventually([&] { return caught_up() && other_caught_up(*primary_); }));
  other.reset();
  for (int i = 0; i < 50; ++i) EXPECT_EQ(p.text({"SET", std::format("b{}", i), "2"}), "OK");
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  Client r(replica_->port());
  EXPECT_EQ(r.text({"REPLICAOF", "NO", "ONE"}), "OK");
  EXPECT_EQ(r.text({"SET", "c", "3"}), "OK");

  // The replica that was behind goes on from the promoted one, in its WAL.
  other = std::make_unique<Node>(dir_ / "other", 3600, std::format("127.0.0.1 {}", replica_->port()));
  ASSERT_TRUE(eventually([&] { return other_caught_up(*replica_); }));
  EXPECT_EQ(other->info("full_syncs"), "0");
  EXPECT_EQ(other->info("partial_syncs"), "1");
  Client o(other->port());
  EXPECT_EQ(o.text({"GET", "a"}), "1");
  EXPECT_EQ(o.text({"GET", "b49"}), "2");
  EXPECT_EQ(o.text({"GET", "c"}), "3");

  // So does the old primary, which wrote nothing since.
  EXPECT_EQ(p.text({"REPLICAOF", "127.0.0.1", std::to_string(replica_->port())}), "OK");
  ASSERT_TRUE(eventually([&] {
    return primary_->info("master_link_status") == "up" &&
           primary_->info("slave_repl_offset") == replica_->info("repl_offset");
  }));
  EXPECT_EQ(primary_->info("full_syncs"), "0");
  EXPECT_EQ(primary_->info("partial_syncs"), "1");
  EXPECT_EQ(p.text({"GET", "c"}), "3");
  // And both go on: their offsets are the new primary's now.
  EXPECT_EQ(r.text({"SET", "d", "4"}), "OK");
  EXPECT_EQ(r.text({"WAIT", "2", "5000"}), "2");
  EXPECT_EQ(o.text({"GET", "d"}), "4");
  other.reset();
  other = std::make_unique<Node>(dir_ / "other", 3600, std::format("127.0.0.1 {}", replica_->port()));
  ASSERT_TRUE(eventually([&] { return other_caught_up(*replica_); }));
  EXPECT_EQ(other->info("full_syncs"), "0");
}

TEST_F(ReplicationTest, AnOldPrimaryThatWroteOnSyncsInFull) {
  Client p(primary_->port());
  EXPECT_EQ(p.text({"SET", "a", "1"}), "OK");
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  Client r(replica_->port());
  EXPECT_EQ(r.text({"REPLICAOF", "NO", "ONE"}), "OK");
  // Written after the promotion: the new primary never had it.
  EXPECT_EQ(p.text({"SET", "lost", "1"}), "OK");
  EXPECT_EQ(p.text({"REPLICAOF", "127.0.0.1", std::to_string(replica_->port())}), "OK");
  ASSERT_TRUE(eventually([&] {
    return primary_->info("master_link_status") == "up" &&
           primary_->info("slave_repl_offset") == replica_->info("repl_offset");
  }));
  EXPECT_EQ(primary_->info("full_syncs"), "1");
  EXPECT_EQ(p.text({"GET", "a"}), "1");
  EXPECT_EQ(p.run({"GET", "lost"}).text, "");
}

TEST_F(ReplicationTest, PrimaryNeedsAKeptWal) {
  primary_.reset();
  primary_ = std::make_unique<Node>(dir_ / "primary2", 0);  // no wal-retention
  start_replica();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_EQ(replica_->info("master_link_status"), "down");
  Value v = Client(primary_->port()).run({"REPLSYNC", "?", "0"});
  EXPECT_TRUE(v.error);
  EXPECT_NE(v.text.find("wal-retention"), std::string::npos);
}

TEST_F(ReplicationTest, ReplicaBehindTheKeptWalSyncsAgain) {
  primary_.reset();
  primary_ = std::make_unique<Node>(dir_ / "small", 3600, "", 1);  // keeps at most 1 byte of old WAL
  Client p(primary_->port());
  EXPECT_EQ(p.text({"SET", "a", "1"}), "OK");
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  replica_.reset();
  // While the replica is away, the WAL it would need goes.
  EXPECT_EQ(p.text({"SET", "b", "2"}), "OK");
  primary_->storage->flush();
  EXPECT_EQ(p.text({"SET", "c", "3"}), "OK");
  primary_->storage->flush();
  ASSERT_TRUE(eventually([&] { return primary_->storage->trim_wal() > 0; }));
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_EQ(replica_->info("full_syncs"), "1");
  Client r(replica_->port());
  EXPECT_EQ(r.text({"GET", "b"}), "2");
  EXPECT_EQ(r.text({"GET", "c"}), "3");
}

TEST_F(ReplicationTest, WaitCountsReplicasThatHaveTheWrites) {
  Client p(primary_->port());
  EXPECT_EQ(p.text({"WAIT", "0", "0"}), "0");  // nothing to wait for
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_EQ(p.text({"SET", "k", "v"}), "OK");
  // Replicas acknowledge as soon as they applied a batch, not once a second.
  auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(p.text({"WAIT", "1", "5000"}), "1");
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(900));
  // Asking for more replicas than there are waits for the timeout.
  start = std::chrono::steady_clock::now();
  EXPECT_EQ(p.text({"WAIT", "2", "200"}), "1");
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(180));
  EXPECT_TRUE(p.run({"WAIT", "1", "-1"}).error);
  Value on_replica = Client(replica_->port()).run({"WAIT", "1", "0"});
  EXPECT_TRUE(on_replica.error);
  EXPECT_NE(on_replica.text.find("replica"), std::string::npos);
  // INFO has what Redis Sentinel reads.
  std::string server = p.text({"INFO", "server"});
  ASSERT_NE(server.find("run_id:"), std::string::npos);
  EXPECT_EQ(server.substr(server.find("run_id:") + 7, 40).find_first_not_of("0123456789abcdef"), std::string::npos);
  EXPECT_EQ(replica_->info("slave_priority"), "100");
  EXPECT_EQ(primary_->info("master_failover_state"), "no-failover");
}

TEST_F(ReplicationTest, FollowingAgainKeepsTheLinkUp) {
  // REPLICAOF while the link streams (what Sentinel sends): the old link must
  // not report itself down once the new one is up.
  Client p(primary_->port());
  EXPECT_EQ(p.text({"SET", "a", "1"}), "OK");
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  Client r(replica_->port());
  EXPECT_EQ(r.text({"REPLICAOF", "127.0.0.1", std::to_string(primary_->port())}), "OK");
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_EQ(replica_->info("partial_syncs"), "1");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // past an acknowledgment tick
  EXPECT_EQ(replica_->info("master_link_status"), "up");
  EXPECT_EQ(p.text({"SET", "b", "2"}), "OK");
  EXPECT_EQ(p.text({"WAIT", "1", "5000"}), "1");
}

TEST_F(ReplicationTest, FailoverSwapsTheRoles) {
  Client p(primary_->port());
  EXPECT_TRUE(p.run({"FAILOVER"}).error);  // no replica yet
  EXPECT_EQ(p.text({"SET", "a", "1"}), "OK");
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_EQ(p.run({"FAILOVER", "TO", "10.9.9.9", "1"}).text, "ERR FAILOVER target HOST and PORT is not a replica.");
  EXPECT_TRUE(p.run({"FAILOVER", "FORCE"}).error);  // FORCE needs a target and a timeout
  EXPECT_EQ(p.text({"FAILOVER", "ABORT"}), "ERR No failover in progress.");
  EXPECT_TRUE(Client(replica_->port()).run({"FAILOVER"}).error);  // only a primary hands over

  EXPECT_EQ(p.text({"FAILOVER", "TIMEOUT", "5000"}), "OK");
  // The replica takes over; the old primary follows it.
  ASSERT_TRUE(eventually([&] { return replica_->info("role") == "master" && primary_->info("role") == "slave"; }))
      << primary_->info("master_failover_state");
  ASSERT_TRUE(eventually([&] { return primary_->info("master_link_status") == "up"; }));
  // It handed over at its last write: it continues, no full sync.
  EXPECT_EQ(primary_->info("full_syncs"), "0");
  EXPECT_EQ(primary_->info("partial_syncs"), "1");
  Client r(replica_->port());
  EXPECT_EQ(r.text({"GET", "a"}), "1");
  EXPECT_EQ(r.text({"SET", "b", "2"}), "OK");
  EXPECT_TRUE(p.run({"SET", "c", "3"}).text.starts_with("READONLY"));
  ASSERT_TRUE(eventually([&] { return Client(primary_->port()).text({"GET", "b"}) == "2"; }));
  EXPECT_EQ(primary_->info("master_failover_state"), "no-failover");
  EXPECT_EQ(primary_->info("master_port"), std::to_string(replica_->port()));
}

TEST_F(ReplicationTest, PromotionHandoffLosesNoWrite) {
  Client p(primary_->port());
  EXPECT_EQ(p.text({"SET", "a", "1"}), "OK");
  start_replica();
  auto other = std::make_unique<Node>(dir_ / "other", 3600, std::format("127.0.0.1 {}", primary_->port()));
  ASSERT_TRUE(eventually([&] {
    return caught_up() && other->info("master_link_status") == "up" &&
           other->info("slave_repl_offset") == primary_->info("repl_offset");
  }));
  Client r(replica_->port());
  EXPECT_EQ(r.text({"CONFIG", "SET", "promotion-handoff", "yes"}), "OK");

  // Writes go on at the primary while the replica is promoted: every one it
  // acknowledged must be on the new primary.
  std::atomic<bool> stop{false};
  std::vector<int> acked;
  std::thread writer([&] {
    Client w(primary_->port());
    for (int i = 0; !stop; ++i) {
      if (w.text({"SET", std::format("w{}", i), "x"}) == "OK") acked.push_back(i);
      else std::this_thread::sleep_for(std::chrono::milliseconds(1));  // READONLY: paused, then a replica
    }
  });
  struct Join {
    std::atomic<bool>& stop;
    std::thread& t;
    ~Join() {
      stop = true;
      if (t.joinable()) t.join();
    }
  } join{stop, writer};
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(r.text({"REPLICAOF", "NO", "ONE"}), "OK");
  // The primary handed over, and follows the promoted replica.
  ASSERT_TRUE(eventually([&] { return replica_->info("role") == "master" && primary_->info("role") == "slave"; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop = true;
  writer.join();
  ASSERT_FALSE(acked.empty());
  for (int i : acked) ASSERT_EQ(r.text({"GET", std::format("w{}", i)}), "x") << "w" << i << " was acknowledged";
  ASSERT_TRUE(eventually([&] { return primary_->info("master_link_status") == "up"; }));
  EXPECT_EQ(primary_->info("full_syncs"), "0");

  // The other replica stopped where the promoted one did: it goes on from it.
  EXPECT_EQ(Client(other->port()).text({"REPLICAOF", "127.0.0.1", std::to_string(replica_->port())}), "OK");
  ASSERT_TRUE(eventually([&] {
    return other->info("master_link_status") == "up" &&
           other->info("slave_repl_offset") == replica_->info("repl_offset");
  }));
  EXPECT_EQ(other->info("full_syncs"), "1");  // its first sync, from the old primary
  EXPECT_EQ(other->info("partial_syncs"), "1");
}

TEST_F(ReplicationTest, FunctionsFollowThePrimary) {
  Client p(primary_->port());
  const std::string lib1 =
      "#!lua name=one\nredis.register_function{function_name = 'get1', flags = {'no-writes'}, "
      "callback = function(keys) return redis.call('GET', keys[1]) end}";
  const std::string lib2 =
      "#!lua name=two\nredis.register_function('set2', function(keys, args) return redis.call('SET', keys[1], "
      "args[1]) end)";
  EXPECT_EQ(p.text({"FUNCTION", "LOAD", lib1}), "one");
  EXPECT_EQ(p.text({"SET", "k", "v"}), "OK");
  // In the full sync.
  start_replica();
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  Client r(replica_->port());
  EXPECT_EQ(r.text({"FCALL_RO", "get1", "1", "k"}), "v");
  // And in the stream, with what the functions write.
  EXPECT_EQ(p.text({"FUNCTION", "LOAD", lib2}), "two");
  EXPECT_EQ(p.text({"FCALL", "set2", "1", "k", "w"}), "OK");
  EXPECT_EQ(p.text({"EVAL", "return redis.call('SET', KEYS[1], ARGV[1])", "1", "e", "1"}), "OK");
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_EQ(r.text({"FCALL", "get1", "1", "k"}), "w");
  EXPECT_EQ(r.text({"GET", "e"}), "1");
  EXPECT_EQ(r.run({"FUNCTION", "LIST"}).items.size(), 2u);
  // The replica refuses to change them, and functions that write fail there.
  EXPECT_TRUE(r.text({"FUNCTION", "DELETE", "one"}).starts_with("READONLY"));
  EXPECT_TRUE(r.text({"FCALL", "set2", "1", "k", "x"}).starts_with("READONLY"));
  EXPECT_EQ(p.text({"FUNCTION", "DELETE", "one"}), "OK");
  ASSERT_TRUE(eventually([&] { return caught_up(); }));
  EXPECT_EQ(r.text({"FCALL_RO", "get1", "1", "k"}), "ERR Function not found");
}

TEST(FirstToEndTest, AFailureEndsTheOtherAndIsRethrown) {
  // operator|| would keep waiting for the timer: an hour.
  asio::io_context io;
  std::string error;
  auto fails = []() -> asio::awaitable<void> {
    asio::steady_timer t(co_await asio::this_coro::executor, std::chrono::milliseconds(10));
    co_await t.async_wait(kUseAwaitable);
    throw std::runtime_error("the primary closed the connection");
  };
  auto waits = []() -> asio::awaitable<void> {
    asio::steady_timer t(co_await asio::this_coro::executor, std::chrono::hours(1));
    co_await t.async_wait(kUseAwaitable);
  };
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        try {
          co_await first_to_end(fails(), waits());
        } catch (const std::exception& e) {
          error = e.what();
        }
      },
      asio::detached);
  auto start = std::chrono::steady_clock::now();
  io.run();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
  EXPECT_EQ(error, "the primary closed the connection");
}
