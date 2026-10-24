// MULTI/EXEC/WATCH, blocking commands (BLPOP...) and Pub/Sub: command
// semantics through execute_redis_command, and waiting and message delivery
// on a real server.
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

// A RESP2/RESP3 reply: a string, or an array (also a RESP3 push).
struct Value {
  std::string text;
  bool array = false, error = false, null = false, push = false;
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
  if (type == '*' || type == '>') {
    v.array = true;
    v.push = type == '>';
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
    v.null = type == '_';
  }
  in = rest;
  return true;
}

// All the replies in `out`, as text items ("nil" for a null).
std::vector<Value> parse_all(std::string_view out) {
  std::vector<Value> values;
  Value v;
  while (!out.empty() && parse(out, v)) values.push_back(v);
  return values;
}

std::vector<std::string> texts(const Value& v) {
  std::vector<std::string> out;
  for (const auto& i : v.items) out.push_back(i.null ? "nil" : i.text);
  return out;
}

// ---- command semantics, without a network

class Commands : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_rt_" + std::to_string(std::random_device{}()));
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
    a_ = b_ = ClientState{};
    ctx_.reset();
    db_.reset();
    cache_.reset();
    storage_.reset();
    fs::remove_all(dir_);
  }

  Value run(ClientState& client, std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, client.proto);
    execute_redis_command(*ctx_, client, views, reply);
    std::string_view in = out;
    Value v;
    if (!parse(in, v)) v.text = "<no reply>";
    return v;
  }
  std::string text(ClientState& client, std::vector<std::string> args) { return run(client, std::move(args)).text; }
  // What a subscriber has been sent so far.
  std::vector<Value> received(ClientState& client) {
    std::string out;
    if (client.subscriber) client.subscriber->take(&out);
    return parse_all(out);
  }

  fs::path dir_;
  Config config_;
  ServerStats stats_;
  ClientState a_, b_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ServerContext> ctx_;
};

}  // namespace

TEST_F(Commands, MultiExecRunsTheQueue) {
  EXPECT_EQ(text(a_, {"MULTI"}), "OK");
  EXPECT_EQ(text(a_, {"SET", "a", "1"}), "QUEUED");
  EXPECT_EQ(text(a_, {"INCR", "a"}), "QUEUED");
  EXPECT_EQ(text(a_, {"GET", "a"}), "QUEUED");
  EXPECT_EQ(text(b_, {"GET", "a"}), "");  // nothing ran yet
  Value r = run(a_, {"EXEC"});
  EXPECT_EQ(texts(r), (std::vector<std::string>{"OK", "2", "2"}));
  EXPECT_EQ(text(b_, {"GET", "a"}), "2");
  // An error inside the block does not undo the rest.
  text(a_, {"MULTI"});
  text(a_, {"SET", "s", "x"});
  text(a_, {"LPUSH", "s", "y"});
  text(a_, {"SET", "t", "1"});
  r = run(a_, {"EXEC"});
  ASSERT_EQ(r.items.size(), 3u);
  EXPECT_TRUE(r.items[1].error);
  EXPECT_EQ(text(b_, {"GET", "t"}), "1");
  // Errors and misuse.
  EXPECT_EQ(text(a_, {"EXEC"}), "ERR EXEC without MULTI");
  EXPECT_EQ(text(a_, {"DISCARD"}), "ERR DISCARD without MULTI");
  text(a_, {"MULTI"});
  EXPECT_EQ(text(a_, {"MULTI"}), "ERR MULTI calls can not be nested");
  EXPECT_EQ(text(a_, {"WATCH", "x"}), "ERR WATCH inside MULTI is not allowed");
  text(a_, {"SET", "d", "1"});
  EXPECT_EQ(text(a_, {"DISCARD"}), "OK");
  EXPECT_EQ(text(b_, {"EXISTS", "d"}), "0");
}

TEST_F(Commands, QueueingErrorsAbortTheBlock) {
  text(a_, {"MULTI"});
  text(a_, {"SET", "a", "1"});
  EXPECT_TRUE(run(a_, {"SET"}).error);         // arity
  EXPECT_TRUE(run(a_, {"NOSUCHCMD"}).error);   // unknown
  EXPECT_EQ(text(a_, {"EXEC"}), "EXECABORT Transaction discarded because of previous errors.");
  EXPECT_EQ(text(b_, {"EXISTS", "a"}), "0");
  EXPECT_EQ(text(a_, {"SET", "a", "2"}), "OK");  // out of MULTI again
}

TEST_F(Commands, MultiKeyCommandsAndBlockingOnesInsideABlock) {
  // These lock keys on their own or read RocksDB directly; inside a block
  // they use the block's locks.
  text(a_, {"MULTI"});
  for (auto cmd : std::vector<std::vector<std::string>>{{"MSET", "a", "1", "b", "2"}, {"RENAME", "a", "c"},
                                                        {"KEYS", "*"}, {"BLPOP", "nolist", "0"},
                                                        {"FLUSHALL"}, {"SET", "z", "9"}}) {
    EXPECT_EQ(text(a_, cmd), "QUEUED");
  }
  Value r = run(a_, {"EXEC"});
  ASSERT_EQ(r.items.size(), 6u);
  EXPECT_EQ(r.items[0].text, "OK");
  EXPECT_EQ(r.items[1].text, "OK");
  EXPECT_EQ(r.items[2].items.size(), 2u);  // b and c
  EXPECT_TRUE(r.items[3].null);            // a blocking command answers at once
  EXPECT_EQ(r.items[4].text, "OK");
  EXPECT_EQ(texts(run(b_, {"KEYS", "*"})), std::vector<std::string>{"z"});
}

TEST_F(Commands, WatchedKeysMakeExecFail) {
  text(a_, {"SET", "k", "1"});
  EXPECT_EQ(text(a_, {"WATCH", "k", "other"}), "OK");
  text(b_, {"SET", "k", "2"});  // another client writes it
  text(a_, {"MULTI"});
  text(a_, {"SET", "k", "3"});
  EXPECT_TRUE(run(a_, {"EXEC"}).null);
  EXPECT_EQ(text(b_, {"GET", "k"}), "2");
  // Unchanged: it runs. EXEC unwatches, so a later write does not matter.
  text(a_, {"WATCH", "k"});
  text(a_, {"MULTI"});
  text(a_, {"SET", "k", "3"});
  EXPECT_EQ(texts(run(a_, {"EXEC"})), std::vector<std::string>{"OK"});
  // Collections count (a field write), and so does FLUSHALL.
  text(a_, {"WATCH", "h"});
  text(b_, {"HSET", "h", "f", "v"});
  text(a_, {"MULTI"});
  EXPECT_TRUE(run(a_, {"EXEC"}).null);
  text(a_, {"WATCH", "h"});
  text(b_, {"FLUSHALL"});
  text(a_, {"MULTI"});
  EXPECT_TRUE(run(a_, {"EXEC"}).null);
  // UNWATCH forgets.
  text(a_, {"WATCH", "k"});
  text(a_, {"UNWATCH"});
  text(b_, {"SET", "k", "4"});
  text(a_, {"MULTI"});
  EXPECT_EQ(texts(run(a_, {"EXEC"})), std::vector<std::string>{});
  EXPECT_EQ(db_->key_watch().keys(), 0u);  // nothing left registered
}

TEST_F(Commands, ExecBlocksAreIsolatedFromEachOther) {
  // One client writes x and y together; others read them together. Inside a
  // block the reads are under the block's locks, so they always agree.
  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0}, reads{0};
  std::thread writer([&] {
    ClientState c;
    for (int i = 0; i < 2000; ++i) {
      run(c, {"MULTI"});
      run(c, {"SET", "x", std::to_string(i)});
      run(c, {"SET", "y", std::to_string(i)});
      run(c, {"EXEC"});
    }
    stop = true;
  });
  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    readers.emplace_back([&] {
      ClientState c;
      while (!stop) {
        run(c, {"MULTI"});
        run(c, {"GET", "x"});
        run(c, {"GET", "y"});
        Value r = run(c, {"EXEC"});
        if (r.items.size() != 2 || r.items[0].text != r.items[1].text) ++mismatches;
        ++reads;
      }
    });
  }
  writer.join();
  for (auto& t : readers) t.join();
  EXPECT_EQ(mismatches.load(), 0);
  EXPECT_GT(reads.load(), 0);
}

TEST_F(Commands, PubSubDeliversToChannelsAndPatterns) {
  EXPECT_EQ(text(a_, {"SUBSCRIBE", "news", "sport"}), "<no reply>");  // replies go to the subscriber
  auto got = received(a_);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(texts(got[0]), (std::vector<std::string>{"subscribe", "news", "1"}));
  EXPECT_EQ(texts(got[1]), (std::vector<std::string>{"subscribe", "sport", "2"}));
  run(a_, {"PSUBSCRIBE", "news.*"});
  EXPECT_EQ(texts(received(a_)[0]), (std::vector<std::string>{"psubscribe", "news.*", "3"}));

  EXPECT_EQ(text(b_, {"PUBLISH", "news", "hello"}), "1");
  EXPECT_EQ(text(b_, {"PUBLISH", "news.tech", "chips"}), "1");
  EXPECT_EQ(text(b_, {"PUBLISH", "nobody", "x"}), "0");
  got = received(a_);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(texts(got[0]), (std::vector<std::string>{"message", "news", "hello"}));
  EXPECT_EQ(texts(got[1]), (std::vector<std::string>{"pmessage", "news.*", "news.tech", "chips"}));

  EXPECT_EQ(texts(run(b_, {"PUBSUB", "CHANNELS"})), (std::vector<std::string>{"news", "sport"}));
  EXPECT_EQ(texts(run(b_, {"PUBSUB", "CHANNELS", "s*"})), std::vector<std::string>{"sport"});
  EXPECT_EQ(texts(run(b_, {"PUBSUB", "NUMSUB", "news", "none"})), (std::vector<std::string>{"news", "1", "none", "0"}));
  EXPECT_EQ(text(b_, {"PUBSUB", "NUMPAT"}), "1");

  // RESP2 while subscribed: only (un)subscribing, PING and QUIT.
  EXPECT_EQ(run(a_, {"GET", "k"}).text.substr(0, 20), "ERR Can't execute 'g");
  EXPECT_EQ(texts(run(a_, {"PING"})), (std::vector<std::string>{"pong", ""}));
  run(a_, {"UNSUBSCRIBE"});
  got = received(a_);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(texts(got[1]), (std::vector<std::string>{"unsubscribe", "sport", "1"}));
  run(a_, {"PUNSUBSCRIBE"});
  EXPECT_EQ(texts(received(a_)[0]), (std::vector<std::string>{"punsubscribe", "news.*", "0"}));
  EXPECT_EQ(text(a_, {"SET", "k", "v"}), "OK");  // back to normal
  EXPECT_EQ(text(b_, {"PUBLISH", "news", "again"}), "0");

  // Shard channels are apart from the others.
  run(a_, {"SSUBSCRIBE", "orders"});
  EXPECT_EQ(texts(received(a_)[0]), (std::vector<std::string>{"ssubscribe", "orders", "1"}));
  EXPECT_EQ(text(b_, {"PUBLISH", "orders", "x"}), "0");
  EXPECT_EQ(text(b_, {"SPUBLISH", "orders", "x"}), "1");
  EXPECT_EQ(texts(received(a_)[0]), (std::vector<std::string>{"smessage", "orders", "x"}));
  EXPECT_EQ(texts(run(b_, {"PUBSUB", "SHARDCHANNELS"})), std::vector<std::string>{"orders"});
}

TEST_F(Commands, PubSubOverResp3AndAcl) {
  run(a_, {"HELLO", "3"});
  run(a_, {"SUBSCRIBE", "ch"});
  auto got = received(a_);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(got[0].push);
  EXPECT_EQ(text(a_, {"SET", "k", "v"}), "OK");  // RESP3: any command
  text(b_, {"PUBLISH", "ch", "m"});
  got = received(a_);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(got[0].push);
  EXPECT_EQ(texts(got[0]), (std::vector<std::string>{"message", "ch", "m"}));

  // Channel permissions: & patterns; PSUBSCRIBE needs one of them literally.
  text(b_, {"ACL", "SETUSER", "app", "on", "nopass", "+@all", "~*", "resetchannels", "&news.*"});
  ClientState app;
  ASSERT_EQ(text(app, {"AUTH", "app", "x"}), "OK");
  EXPECT_EQ(text(app, {"PUBLISH", "news.1", "x"}), "0");
  EXPECT_EQ(text(app, {"PUBLISH", "other", "x"}), "NOPERM No permissions to access a channel");
  EXPECT_EQ(text(app, {"SUBSCRIBE", "other"}), "NOPERM No permissions to access a channel");
  EXPECT_EQ(text(app, {"PSUBSCRIBE", "n*"}), "NOPERM No permissions to access a channel");
  run(app, {"PSUBSCRIBE", "news.*"});
  EXPECT_EQ(texts(received(app)[0]), (std::vector<std::string>{"psubscribe", "news.*", "1"}));
  // Categories Redis has and datadam lacks are accepted (and cover nothing).
  EXPECT_EQ(text(b_, {"ACL", "SETUSER", "app", "-@scripting", "+@pubsub", "-@blocking"}), "OK");
}

// ---- on a real server

namespace {

// A blocking client connection.
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
  Value read() {
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
  Value run(const std::vector<std::string>& args) {
    send(args);
    return read();
  }
  std::string text(const std::vector<std::string>& args) { return run(args).text; }
  void close() { socket_.close(); }

 private:
  asio::io_context io_;
  tcp::socket socket_;
  std::string buf_;
};

class Live : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_live_" + std::to_string(std::random_device{}()));
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
    config_.bind = "127.0.0.1";
    config_.io_threads = 2;
    config_.memcache_port = 0;
    config_.read_threads = 4;
    ctx_ = std::make_unique<ServerContext>(*db_, config_, stats_, 0);
    server_ = std::make_unique<Server>(*ctx_);
    server_->start(true);
    thread_ = std::thread([this] { server_->run(); });
  }
  void TearDown() override {
    server_->stop();
    thread_.join();
    server_.reset();
    ctx_.reset();
    db_.reset();
    cache_.reset();
    storage_.reset();
    fs::remove_all(dir_);
  }
  uint16_t port() const { return server_->port(); }

  fs::path dir_;
  Config config_;
  ServerStats stats_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ServerContext> ctx_;
  std::unique_ptr<Server> server_;
  std::thread thread_;
};

}  // namespace

TEST_F(Live, BlpopWaitsForAPush) {
  Client waiter(port()), pusher(port());
  auto start = std::chrono::steady_clock::now();
  waiter.send({"BLPOP", "q1", "q2", "5"});
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(pusher.text({"RPUSH", "q2", "job"}), "1");
  Value r = waiter.read();
  EXPECT_EQ(texts(r), (std::vector<std::string>{"q2", "job"}));
  EXPECT_GE(std::chrono::steady_clock::now() - start, 90ms);
  EXPECT_EQ(pusher.text({"EXISTS", "q2"}), "0");
  // Data already there: no wait. Timeout: nil, after about the timeout.
  pusher.text({"RPUSH", "q1", "a"});
  EXPECT_EQ(texts(waiter.run({"BRPOP", "q1", "0"})), (std::vector<std::string>{"q1", "a"}));
  start = std::chrono::steady_clock::now();
  EXPECT_TRUE(waiter.run({"BLPOP", "q1", "0.2"}).null);
  auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_GE(waited, 180ms);
  EXPECT_LT(waited, 2s);
  // The pipeline after a blocked command waits for it, replies in order.
  waiter.send({"BLPOP", "q1", "5"});
  waiter.send({"PING"});
  std::this_thread::sleep_for(50ms);
  pusher.text({"LPUSH", "q1", "b"});
  EXPECT_EQ(texts(waiter.read()), (std::vector<std::string>{"q1", "b"}));
  EXPECT_EQ(waiter.read().text, "PONG");
  EXPECT_TRUE(waiter.run({"BLPOP", "q1", "nope"}).error);
  EXPECT_TRUE(waiter.run({"BLPOP", "q1", "-1"}).error);
}

TEST_F(Live, EachElementGoesToOneWaiter) {
  Client w1(port()), w2(port()), pusher(port());
  w1.send({"BLPOP", "q", "5"});
  w2.send({"BLPOP", "q", "5"});
  std::this_thread::sleep_for(100ms);
  pusher.text({"RPUSH", "q", "x", "y"});
  auto a = texts(w1.read()), b = texts(w2.read());
  ASSERT_EQ(a.size(), 2u);
  ASSERT_EQ(b.size(), 2u);
  EXPECT_NE(a[1], b[1]);  // x to one, y to the other
  EXPECT_EQ(pusher.text({"LLEN", "q"}), "0");
}

TEST_F(Live, BlmoveAndBzpopWait) {
  Client w(port()), p(port());
  w.send({"BLMOVE", "src", "dst", "LEFT", "RIGHT", "5"});
  std::this_thread::sleep_for(50ms);
  p.text({"RPUSH", "src", "item"});
  EXPECT_EQ(w.read().text, "item");
  EXPECT_EQ(texts(p.run({"LRANGE", "dst", "0", "-1"})), std::vector<std::string>{"item"});
  w.send({"BZPOPMIN", "z", "5"});
  std::this_thread::sleep_for(50ms);
  p.text({"ZADD", "z", "2", "b", "1", "a"});
  EXPECT_EQ(texts(w.read()), (std::vector<std::string>{"z", "a", "1"}));
  w.send({"BLMPOP", "5", "2", "l1", "l2", "RIGHT", "COUNT", "2"});
  std::this_thread::sleep_for(50ms);
  p.text({"RPUSH", "l2", "1", "2", "3"});
  Value r = w.read();
  ASSERT_EQ(r.items.size(), 2u);
  EXPECT_EQ(r.items[0].text, "l2");
  EXPECT_EQ(texts(r.items[1]), (std::vector<std::string>{"3", "2"}));
  // Wrong type is an error, not a wait.
  p.text({"SET", "str", "x"});
  EXPECT_TRUE(w.run({"BLPOP", "str", "1"}).error);
}

TEST_F(Live, AClientThatLeavesWhileBlockedTakesNothing) {
  {
    Client gone(port());
    gone.send({"BLPOP", "q", "0"});
    std::this_thread::sleep_for(50ms);
    gone.close();
  }
  std::this_thread::sleep_for(100ms);
  Client p(port());
  p.text({"RPUSH", "q", "x"});
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(p.text({"LLEN", "q"}), "1");
}

TEST_F(Live, PublishReachesSubscribersAtOnce) {
  Client sub(port()), pub(port());
  EXPECT_EQ(texts(sub.run({"SUBSCRIBE", "ch"})), (std::vector<std::string>{"subscribe", "ch", "1"}));
  EXPECT_EQ(pub.text({"PUBLISH", "ch", "one"}), "1");
  EXPECT_EQ(texts(sub.read()), (std::vector<std::string>{"message", "ch", "one"}));
  // Many, from several publishers: all arrive, each publisher's in order.
  std::vector<std::thread> publishers;
  for (int t = 0; t < 4; ++t) {
    publishers.emplace_back([&, t] {
      Client c(port());
      for (int i = 0; i < 200; ++i) c.text({"PUBLISH", "ch", std::format("{}:{}", t, i)});
    });
  }
  for (auto& t : publishers) t.join();
  std::vector<int> next(4, 0);
  for (int n = 0; n < 800; ++n) {
    auto m = texts(sub.read());
    ASSERT_EQ(m.size(), 3u);
    int t = m[2][0] - '0', i = std::stoi(m[2].substr(2));
    EXPECT_EQ(i, next[static_cast<size_t>(t)]++);
  }
  // Leaving: the subscription goes with the connection.
  EXPECT_EQ(texts(sub.run({"UNSUBSCRIBE"})), (std::vector<std::string>{"unsubscribe", "ch", "0"}));
  EXPECT_EQ(sub.text({"PING"}), "PONG");
  sub.run({"SUBSCRIBE", "ch"});
  sub.close();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(texts(pub.run({"PUBSUB", "NUMSUB", "ch"})), (std::vector<std::string>{"ch", "0"}));
}

TEST_F(Live, MultiExecOverAConnection) {
  Client c(port()), other(port());
  c.send({"MULTI"});
  c.send({"INCR", "n"});
  c.send({"INCR", "n"});
  c.send({"EXEC"});
  EXPECT_EQ(c.read().text, "OK");
  EXPECT_EQ(c.read().text, "QUEUED");
  EXPECT_EQ(c.read().text, "QUEUED");
  EXPECT_EQ(texts(c.read()), (std::vector<std::string>{"1", "2"}));
  // WATCH across connections.
  c.text({"WATCH", "n"});
  other.text({"INCR", "n"});
  c.text({"MULTI"});
  c.text({"INCR", "n"});
  EXPECT_TRUE(c.run({"EXEC"}).null);
  EXPECT_EQ(other.text({"GET", "n"}), "3");
}
