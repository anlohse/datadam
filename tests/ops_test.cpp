// Operations: uncommitted writes stay invisible, backups (BACKUP / BGSAVE),
// SLOWLOG, OBJECT / TOUCH / MEMORY, and on a real server CLIENT LIST / KILL,
// MONITOR and SHUTDOWN.
#include <gtest/gtest.h>

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <random>
#include <thread>

#include "cache/object_cache.h"
#include "db/database.h"
#include "server/redis_commands.h"
#include "server/server.h"
#include "storage/rocks_storage.h"

using namespace datadam;
using asio::ip::tcp;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// A RESP2 reply: a string, or an array.
struct Value {
  std::string text;
  bool array = false, error = false, null = false;
  std::vector<Value> items;
};

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
    if (n < 0) v.null = true;
    for (int i = 0; i < n; ++i) {
      Value item;
      if (!parse(rest, item)) return false;
      v.items.push_back(std::move(item));
    }
  } else if (type == '$') {
    int n = std::stoi(std::string(head));
    if (n < 0) {
      v.null = true;
    } else {
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

std::vector<std::string> texts(const Value& v) {
  std::vector<std::string> out;
  for (const auto& i : v.items) out.push_back(i.null ? "nil" : i.text);
  return out;
}

// One database (and command context) in a directory of its own.
struct Instance {
  explicit Instance(const fs::path& dir) {
    StorageOptions so;
    so.dir = dir.string();
    so.fsync = FsyncPolicy::No;
    storage = RocksStorage::open(so);
    CacheOptions co;
    co.max_memory = 4 << 20;
    co.min_memory = 0;
    co.shards = 2;
    cache = std::make_unique<ObjectCache>(co);
    db = std::make_unique<Database>(*storage, *cache);
    ctx = std::make_unique<ServerContext>(*db, config, stats, 1'000'000);
  }
  ~Instance() {
    ctx.reset();
    db.reset();
    cache.reset();
    storage.reset();
  }
  Value run(ClientState& client, std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, client.proto);
    execute_redis_command(*ctx, client, views, reply);
    std::string_view in = out;
    Value v;
    if (!parse(in, v)) v.text = "<no reply>";
    return v;
  }

  Config config;
  ServerStats stats;
  std::unique_ptr<RocksStorage> storage;
  std::unique_ptr<ObjectCache> cache;
  std::unique_ptr<Database> db;
  std::unique_ptr<ServerContext> ctx;
};

class Ops : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("datadam_ops_" + std::to_string(std::random_device{}()));
    fs::create_directories(root_);
    db_ = std::make_unique<Instance>(root_ / "data");
  }
  void TearDown() override {
    a_ = ClientState{};
    db_.reset();
    fs::remove_all(root_);
  }
  Value run(std::vector<std::string> args) { return db_->run(a_, std::move(args)); }
  std::string text(std::vector<std::string> args) { return run(std::move(args)).text; }
  // A field of INFO persistence.
  std::string persistence(const std::string& field) {
    std::string all = text({"INFO", "persistence"});
    size_t at = all.find(field + ":");
    if (at == std::string::npos) return {};
    size_t end = all.find("\r\n", at);
    return all.substr(at + field.size() + 1, end - at - field.size() - 1);
  }

  fs::path root_;
  std::unique_ptr<Instance> db_;
  ClientState a_;
};

}  // namespace

TEST_F(Ops, UncommittedWritesAreInvisibleToOtherClients) {
  text({"SET", "k", "old"});
  std::atomic<bool> done{false};
  std::string seen;
  std::thread reader;
  {
    Database::WriteBatchScope batch(*db_->db);
    EXPECT_EQ(text({"SET", "k", "new"}), "OK");
    EXPECT_EQ(text({"GET", "k"}), "new");  // the writer's own pipeline sees it
    reader = std::thread([&] {
      ClientState other;
      seen = db_->run(other, {"GET", "k"}).text;
      done = true;
    });
    std::this_thread::sleep_for(100ms);
    // Not committed yet: the other client waits instead of reading it early.
    EXPECT_FALSE(done.load());
    batch.commit();
  }
  reader.join();
  EXPECT_EQ(seen, "new");
  // The same inside MULTI/EXEC, for every key of the block until it ends.
  text({"MULTI"});
  text({"SET", "x", "1"});
  text({"SET", "y", "1"});
  EXPECT_EQ(texts(run({"EXEC"})), (std::vector<std::string>{"OK", "OK"}));
  ClientState other;
  EXPECT_EQ(db_->run(other, {"MGET", "x", "y"}).items.size(), 2u);
}

TEST_F(Ops, BackupIsAConsistentCopy) {
  text({"SET", "s", "v"});
  text({"HSET", "h", "f1", "a", "f2", "b"});
  text({"JSON.SET", "j", "$", R"({"n":1})"});
  fs::path copy = root_ / "copies" / "one";
  // Where the server writes is not the client's to choose, unless
  // enable-protected-configs says so (as in Redis 7).
  EXPECT_NE(text({"BACKUP", copy.string()}).find("backup-dir"), std::string::npos);
  EXPECT_NE(text({"CONFIG", "SET", "backup-dir", root_.string()}).find("can't set protected config"),
            std::string::npos);
  db_->config.enable_protected_configs = "yes";
  EXPECT_EQ(text({"BACKUP", copy.string()}), "OK");
  EXPECT_TRUE(run({"BACKUP", copy.string()}).error);  // the target must not exist
  EXPECT_EQ(persistence("last_backup_path"), copy.string());
  EXPECT_GE(std::stoll(text({"LASTSAVE"})), 1000);
  text({"SET", "s", "changed later"});
  {
    // The copy opens as a database, with the data of the moment it was made.
    Instance restored(copy);
    ClientState c;
    EXPECT_EQ(restored.run(c, {"GET", "s"}).text, "v");
    EXPECT_EQ(texts(restored.run(c, {"HGETALL", "h"})), (std::vector<std::string>{"f1", "a", "f2", "b"}));
    EXPECT_EQ(restored.run(c, {"JSON.GET", "j", "$.n"}).text, "[1]");
  }

  // BGSAVE: a flush without backup-dir; a checkpoint per call with it.
  EXPECT_EQ(text({"BGSAVE"}), "Background saving started");
  fs::path dir = root_ / "backups";
  EXPECT_EQ(text({"CONFIG", "SET", "backup-dir", dir.string(), "backup-keep", "2"}), "OK");
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(text({"BGSAVE"}), "Background saving started");
    for (int w = 0; w < 500 && persistence("rdb_bgsave_in_progress") != "0"; ++w) std::this_thread::sleep_for(10ms);
    EXPECT_EQ(persistence("rdb_last_bgsave_status"), "ok");
  }
  size_t kept = 0;
  for (const auto& e : fs::directory_iterator(dir)) kept += e.is_directory() ? 1 : 0;
  EXPECT_EQ(kept, 2u);  // the oldest went

  // Protected: BACKUP inside backup-dir only.
  db_->config.enable_protected_configs = "no";
  EXPECT_EQ(text({"BACKUP", "named"}), "OK");
  EXPECT_TRUE(fs::exists(dir / "named"));
  EXPECT_EQ(text({"BACKUP", (dir / "sub" / "abs").string()}), "OK");
  for (const std::string& outside : {std::string("../escape"), (root_ / "elsewhere").string(), dir.string(),
                                     std::string("sub/../../escape")}) {
    EXPECT_NE(text({"BACKUP", outside}).find("not inside backup-dir"), std::string::npos) << outside;
  }
  EXPECT_FALSE(fs::exists(root_ / "escape"));
}

TEST_F(Ops, SlowlogRecordsAndRedacts) {
  EXPECT_EQ(text({"CONFIG", "SET", "slowlog-log-slower-than", "0", "slowlog-max-len", "3"}), "OK");
  text({"SLOWLOG", "RESET"});
  text({"SET", "k", std::string(200, 'x')});
  text({"AUTH", "default", "secret"});
  Value log = run({"SLOWLOG", "GET", "2"});
  ASSERT_EQ(log.items.size(), 2u);
  // Newest first: AUTH, with its password hidden; then SET, its long argument cut.
  EXPECT_EQ(texts(log.items[0].items[3]), (std::vector<std::string>{"AUTH", "(redacted)"}));
  auto set_args = texts(log.items[1].items[3]);
  ASSERT_EQ(set_args.size(), 3u);
  EXPECT_EQ(set_args[2], std::string(128, 'x') + "... (72 more bytes)");
  text({"PING"});
  text({"PING"});
  EXPECT_EQ(text({"SLOWLOG", "LEN"}), "3");  // capped by slowlog-max-len
  EXPECT_EQ(text({"SLOWLOG", "RESET"}), "OK");
  EXPECT_EQ(text({"CONFIG", "SET", "slowlog-log-slower-than", "-1"}), "OK");
  text({"PING"});
  EXPECT_EQ(text({"SLOWLOG", "LEN"}), "1");  // the RESET itself was logged, nothing after
}

TEST_F(Ops, ObjectTouchAndMemory) {
  text({"SET", "n", "12345"});
  text({"SET", "s", "hello"});
  text({"SET", "big", std::string(100, 'x')});
  text({"HSET", "h", "f", "v"});
  text({"RPUSH", "l", "a"});
  EXPECT_EQ(text({"OBJECT", "ENCODING", "n"}), "int");
  EXPECT_EQ(text({"OBJECT", "ENCODING", "s"}), "embstr");
  EXPECT_EQ(text({"OBJECT", "ENCODING", "big"}), "raw");
  EXPECT_EQ(text({"OBJECT", "ENCODING", "h"}), "listpack");
  EXPECT_EQ(text({"OBJECT", "ENCODING", "l"}), "listpack");
  EXPECT_TRUE(run({"OBJECT", "ENCODING", "missing"}).null);
  EXPECT_EQ(text({"OBJECT", "REFCOUNT", "s"}), "1");
  EXPECT_EQ(text({"OBJECT", "IDLETIME", "s"}), "0");
  EXPECT_TRUE(run({"OBJECT", "FREQ", "s"}).error);
  EXPECT_EQ(text({"TOUCH", "n", "s", "missing"}), "2");

  EXPECT_GT(std::stoll(text({"MEMORY", "USAGE", "big"})), 100);
  for (int i = 0; i < 300; ++i) text({"HSET", "wide", std::format("field{}", i), "value"});
  int64_t wide = std::stoll(text({"MEMORY", "USAGE", "wide"}));
  int64_t all = std::stoll(text({"MEMORY", "USAGE", "wide", "SAMPLES", "0"}));
  EXPECT_GT(wide, 300 * 10);
  EXPECT_NEAR(static_cast<double>(wide), static_cast<double>(all), all * 0.2);
  EXPECT_EQ(text({"OBJECT", "ENCODING", "wide"}), "hashtable");
  EXPECT_TRUE(run({"MEMORY", "USAGE", "missing"}).null);
  EXPECT_TRUE(run({"SHUTDOWN"}).error);  // no server to stop here
  EXPECT_EQ(text({"SHUTDOWN", "ABORT"}), "ERR No shutdown in progress.");
}

// ---- on a real server

namespace {

class Client {
 public:
  explicit Client(uint16_t port) : socket_(io_) {
    socket_.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  }
  void send(const std::vector<std::string>& args) {
    std::string out = std::format("*{}\r\n", args.size());
    for (const auto& a : args) out += std::format("${}\r\n{}\r\n", a.size(), a);
    asio::write(socket_, asio::buffer(out));
  }
  // nullopt once the server closed the connection.
  std::optional<Value> read() {
    for (;;) {
      std::string_view view = buf_;
      Value v;
      if (parse(view, v)) {
        buf_.erase(0, buf_.size() - view.size());
        return v;
      }
      char chunk[4096];
      asio::error_code ec;
      size_t n = socket_.read_some(asio::buffer(chunk), ec);
      if (ec) return std::nullopt;
      buf_.append(chunk, n);
    }
  }
  Value run(const std::vector<std::string>& args) {
    send(args);
    auto v = read();
    if (v) return *v;
    Value closed;
    closed.text = "<closed>";
    return closed;
  }
  std::string text(const std::vector<std::string>& args) { return run(args).text; }

 private:
  asio::io_context io_;
  tcp::socket socket_;
  std::string buf_;
};

class LiveOps : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("datadam_liveops_" + std::to_string(std::random_device{}()));
    db_ = std::make_unique<Instance>(root_);
    db_->config.bind = "127.0.0.1";
    db_->config.io_threads = 2;
    db_->config.memcache_port = 0;
    db_->config.metrics_port = 1;  // any: start(true) takes a free one
    db_->config.read_threads = 4;
    server_ = std::make_unique<Server>(*db_->ctx);
    server_->start(true);
    thread_ = std::thread([this] { server_->run(); });
  }
  void TearDown() override {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
    server_.reset();
    db_.reset();
    fs::remove_all(root_);
  }
  uint16_t port() const { return server_->port(); }

  fs::path root_;
  std::unique_ptr<Instance> db_;
  std::unique_ptr<Server> server_;
  std::thread thread_;
};

}  // namespace

// One HTTP request to the metrics port; the whole response.
std::string http(uint16_t port, const std::string& request) {
  asio::io_context io;
  tcp::socket s(io);
  s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  asio::write(s, asio::buffer(request));
  std::string out;
  asio::error_code ec;
  char chunk[4096];
  while (!ec) out.append(chunk, s.read_some(asio::buffer(chunk), ec));
  return out;
}

TEST_F(LiveOps, MetricsPortServesPrometheusHealthAndReady) {
  Client c(port());
  c.text({"SET", "k", "v"});
  c.text({"GET", "k"});
  std::string r = http(server_->metrics_port(), "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_TRUE(r.starts_with("HTTP/1.1 200 OK\r\n")) << r.substr(0, 200);
  EXPECT_NE(r.find("Content-Type: text/plain; version=0.0.4"), std::string::npos);
  size_t body = r.find("\r\n\r\n") + 4;
  size_t length = r.find("Content-Length: ");
  ASSERT_NE(length, std::string::npos);
  EXPECT_EQ(std::stoul(r.substr(length + 16)), r.size() - body);
  EXPECT_NE(r.find("datadam_commands_total{cmd=\"get\"} 1\n"), std::string::npos);
  EXPECT_NE(r.find("datadam_connected_clients 1\n"), std::string::npos);
  // The client's traffic is counted.
  std::string stats = c.text({"INFO", "stats"});
  EXPECT_EQ(stats.find("total_net_input_bytes:0\r\n"), std::string::npos) << stats;
  EXPECT_EQ(stats.find("total_net_output_bytes:0\r\n"), std::string::npos) << stats;

  EXPECT_TRUE(http(server_->metrics_port(), "GET /health HTTP/1.0\r\n\r\n").ends_with("\r\n\r\nOK\n"));
  EXPECT_TRUE(http(server_->metrics_port(), "GET /ready HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 200 OK"));
  std::string head = http(server_->metrics_port(), "HEAD /metrics HTTP/1.1\r\n\r\n");
  EXPECT_TRUE(head.ends_with("\r\n\r\n")) << head;
  EXPECT_TRUE(http(server_->metrics_port(), "GET /x HTTP/1.1\n\n").starts_with("HTTP/1.1 404"));
  // Not HTTP: an answer, not a hang.
  EXPECT_TRUE(http(server_->metrics_port(), "PING\r\n\r\n").starts_with("HTTP/1.1 400"));
  // The Redis port still answers while a metrics client sends nothing.
  asio::io_context io;
  tcp::socket idle(io);
  idle.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server_->metrics_port()));
  EXPECT_EQ(c.text({"PING"}), "PONG");
}

TEST_F(LiveOps, ClientListAndKill) {
  Client a(port()), b(port());
  a.text({"CLIENT", "SETNAME", "alpha"});
  b.text({"CLIENT", "SETNAME", "beta"});
  std::string id_b = b.text({"CLIENT", "ID"});
  std::string list = a.text({"CLIENT", "LIST"});
  EXPECT_NE(list.find("name=alpha"), std::string::npos) << list;
  EXPECT_NE(list.find("name=beta"), std::string::npos) << list;
  EXPECT_NE(list.find("cmd=client"), std::string::npos);
  std::string only = a.text({"CLIENT", "LIST", "ID", id_b});
  EXPECT_EQ(std::count(only.begin(), only.end(), '\n'), 1);
  EXPECT_NE(only.find("name=beta"), std::string::npos);

  // A blocked client shows it.
  b.send({"BLPOP", "q", "5"});
  std::this_thread::sleep_for(100ms);
  EXPECT_NE(a.text({"CLIENT", "LIST", "ID", id_b}).find("flags=b"), std::string::npos);
  EXPECT_NE(a.text({"INFO", "clients"}).find("blocked_clients:1"), std::string::npos);

  EXPECT_EQ(a.text({"CLIENT", "KILL", "ID", id_b}), "1");
  EXPECT_FALSE(b.read().has_value());  // closed
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(a.text({"CLIENT", "LIST"}).find("name=beta"), std::string::npos);
  EXPECT_EQ(a.text({"CLIENT", "KILL", "ADDR", "1.2.3.4:5"}), "0");
  EXPECT_EQ(a.text({"CLIENT", "KILL", "1.2.3.4:5"}), "ERR No such client");
  EXPECT_EQ(a.text({"CLIENT", "KILL", "ID", "1", "SKIPME", "maybe"}), "ERR syntax error");
}

TEST_F(LiveOps, MonitorShowsOtherClientsCommands) {
  Client m(port()), b(port());
  EXPECT_EQ(m.text({"MONITOR"}), "OK");
  b.text({"SET", "k", "v w"});
  b.text({"AUTH", "default", "secret"});
  auto line = m.read();
  ASSERT_TRUE(line);
  EXPECT_NE(line->text.find(R"("SET" "k" "v w")"), std::string::npos) << line->text;
  EXPECT_NE(line->text.find("[0 127.0.0.1:"), std::string::npos);
  line = m.read();
  ASSERT_TRUE(line);
  EXPECT_NE(line->text.find(R"x("AUTH" "(redacted)")x"), std::string::npos) << line->text;
}

TEST_F(LiveOps, ShutdownStopsTheServer) {
  Client c(port());
  c.text({"SET", "k", "v"});
  c.send({"SHUTDOWN", "SAVE"});
  thread_.join();  // run() returned, as when main() is told to stop
  server_.reset();
  EXPECT_FALSE(c.read().has_value());  // no reply: the connection is closed
}

TEST_F(Ops, ProtectedModeRefusesRemoteClientsWithoutAPassword) {
  ServerContext& ctx = *db_->ctx;
  EXPECT_TRUE(protected_mode_refuses(ctx, "10.1.2.3"));
  EXPECT_TRUE(protected_mode_refuses(ctx, "2001:db8::1"));
  EXPECT_TRUE(protected_mode_refuses(ctx, "::ffff:10.1.2.3"));
  EXPECT_FALSE(protected_mode_refuses(ctx, "127.0.0.1"));
  EXPECT_FALSE(protected_mode_refuses(ctx, "127.8.9.10"));
  EXPECT_FALSE(protected_mode_refuses(ctx, "::1"));
  EXPECT_FALSE(protected_mode_refuses(ctx, "::ffff:127.0.0.1"));
  EXPECT_FALSE(protected_mode_refuses(ctx, "10.1.2.3", true));  // a verified TLS client certificate
  // A password for the default user lifts it, and so does turning it off.
  EXPECT_EQ(text({"CONFIG", "SET", "requirepass", "pw"}), "OK");
  EXPECT_FALSE(protected_mode_refuses(ctx, "10.1.2.3"));
  EXPECT_EQ(text({"AUTH", "pw"}), "OK");
  EXPECT_EQ(text({"CONFIG", "SET", "requirepass", ""}), "OK");
  EXPECT_TRUE(protected_mode_refuses(ctx, "10.1.2.3"));
  Value got = run({"CONFIG", "GET", "protected-mode"});
  ASSERT_EQ(got.items.size(), 2u);
  EXPECT_EQ(got.items[1].text, "yes");
  EXPECT_EQ(text({"CONFIG", "SET", "protected-mode", "no"}), "OK");
  EXPECT_FALSE(protected_mode_refuses(ctx, "10.1.2.3"));
}

TEST_F(Ops, InfoShowsRocksDbWriteStalls) {
  for (const char* field : {"rocksdb_write_stall_delays", "rocksdb_write_stall_stops", "rocksdb_stall_memtable_delays",
                            "rocksdb_stall_l0_stops", "rocksdb_write_stopped", "rocksdb_delayed_write_rate",
                            "rocksdb_immutable_memtables", "rocksdb_pending_compaction_bytes",
                            "rocksdb_running_flushes", "rocksdb_running_compactions"}) {
    EXPECT_EQ(persistence(field), "0") << field;  // an idle, fresh database
  }
}
