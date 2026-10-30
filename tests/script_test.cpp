// Lua scripting: EVAL / EVALSHA / SCRIPT / FUNCTION / FCALL through
// execute_redis_command, and atomicity and SCRIPT KILL on a real server.
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
#include "server/scripting.h"
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

class Scripts : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_lua_" + std::to_string(std::random_device{}()));
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
    a_ = ClientState{};
    ctx_.reset();
    db_.reset();
    cache_.reset();
    storage_.reset();
    fs::remove_all(dir_);
  }

  Value run(std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, a_.proto);
    execute_redis_command(*ctx_, a_, views, reply);
    std::string_view in = out;
    Value v;
    if (!parse(in, v)) v.text = "<no reply>";
    return v;
  }
  std::string text(std::vector<std::string> args) { return run(std::move(args)).text; }
  // EVAL body numkeys=keys.size() keys... args...
  Value eval(const std::string& body, std::vector<std::string> keys = {}, std::vector<std::string> args = {}) {
    std::vector<std::string> cmd{"EVAL", body, std::to_string(keys.size())};
    cmd.insert(cmd.end(), keys.begin(), keys.end());
    cmd.insert(cmd.end(), args.begin(), args.end());
    return run(std::move(cmd));
  }

  fs::path dir_;
  Config config_;
  ServerStats stats_;
  ClientState a_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ServerContext> ctx_;
};

}  // namespace

TEST_F(Scripts, ConvertsReturnValuesAsRedisDoes) {
  EXPECT_EQ(eval("return 1").text, "1");
  EXPECT_EQ(eval("return 3.99").text, "3");  // numbers become integers
  EXPECT_EQ(eval("return 'hello'").text, "hello");
  EXPECT_EQ(eval("return true").text, "1");
  EXPECT_TRUE(eval("return false").null);
  EXPECT_TRUE(eval("return nil").null);
  Value r = eval("return {1, 'two', {3, 'four'}, nil, 'after nil'}");
  ASSERT_EQ(r.items.size(), 3u);  // an array stops at the first nil
  EXPECT_EQ(r.items[1].text, "two");
  EXPECT_EQ(texts(r.items[2]), (std::vector<std::string>{"3", "four"}));
  EXPECT_EQ(eval("return {err = 'MYERR something failed'}").text, "MYERR something failed");
  EXPECT_TRUE(eval("return redis.error_reply('ERR boom')").error);
  EXPECT_EQ(eval("return redis.status_reply('FINE')").text, "FINE");
  EXPECT_EQ(texts(eval("return {KEYS[1], KEYS[2], ARGV[1]}", {"k1", "k2"}, {"a1"})),
            (std::vector<std::string>{"k1", "k2", "a1"}));
  EXPECT_EQ(eval("return #ARGV", {}, {"a", "b", "c"}).text, "3");
}

TEST_F(Scripts, CallsCommands) {
  EXPECT_EQ(eval("return redis.call('SET', KEYS[1], ARGV[1])", {"k"}, {"v"}).text, "OK");
  EXPECT_EQ(text({"GET", "k"}), "v");
  EXPECT_EQ(eval("return redis.call('GET', KEYS[1])", {"k"}).text, "v");
  // A missing value is false in Lua.
  EXPECT_EQ(eval("return redis.call('GET', 'nope') == false and 'missing' or 'there'").text, "missing");
  // Numbers as arguments, and integer replies as numbers.
  eval("redis.call('SET', 'n', 41) return redis.call('INCR', 'n') + 1");
  EXPECT_EQ(text({"GET", "n"}), "42");
  // Arrays.
  text({"RPUSH", "l", "a", "b", "c"});
  EXPECT_EQ(texts(eval("return redis.call('LRANGE', 'l', 0, -1)")), (std::vector<std::string>{"a", "b", "c"}));
  // Status replies are {ok = ...} tables.
  EXPECT_EQ(eval("return redis.call('SET', 'x', 1).ok").text, "OK");
  // An error raised by redis.call ends the script; redis.pcall returns it.
  Value e = eval("return redis.call('LPUSH', 'k', 'x')");
  EXPECT_TRUE(e.error);
  EXPECT_TRUE(e.text.starts_with("WRONGTYPE")) << e.text;
  EXPECT_NE(e.text.find("script: "), std::string::npos) << e.text;
  e = eval("local r = redis.pcall('LPUSH', 'k', 'x') return r.err");
  EXPECT_TRUE(e.text.starts_with("WRONGTYPE")) << e.text;
  EXPECT_TRUE(eval("return redis.call('NOSUCHCMD')").text.starts_with("ERR Unknown Redis command"));
  EXPECT_TRUE(eval("return redis.call('GET', {})").text.find("must be strings or integers") != std::string::npos);
  // Commands a script may not call.
  for (const char* cmd : {"MULTI", "EVAL", "SUBSCRIBE", "WATCH", "CLIENT"}) {
    EXPECT_NE(eval(std::format("return redis.call('{}', 'x')", cmd)).text.find("not allowed from script"),
              std::string::npos)
        << cmd;
  }
  // Script errors.
  e = eval("error('my failure')");
  EXPECT_TRUE(e.error);
  EXPECT_NE(e.text.find("my failure"), std::string::npos) << e.text;
  EXPECT_TRUE(eval("return +").text.starts_with("ERR Error compiling script"));
}

TEST_F(Scripts, EvalshaAndTheScriptCache) {
  std::string body = "return ARGV[1] .. '!'";
  std::string sha = text({"SCRIPT", "LOAD", body});
  EXPECT_EQ(sha, sha1_hex(body));
  EXPECT_EQ(sha.size(), 40u);
  EXPECT_EQ(text({"EVALSHA", sha, "0", "hi"}), "hi!");
  std::string upper = sha;
  for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  EXPECT_EQ(text({"EVALSHA", upper, "0", "yo"}), "yo!");
  EXPECT_EQ(texts(run({"SCRIPT", "EXISTS", sha, "0000000000000000000000000000000000000000"})),
            (std::vector<std::string>{"1", "0"}));
  // EVAL caches the script too.
  eval("return 7");
  EXPECT_EQ(text({"EVALSHA", sha1_hex("return 7"), "0"}), "7");
  // Scripts only EVAL loaded are evicted beyond 500, least recently used first; loaded ones stay.
  for (size_t i = 0; i < ScriptEngine::kMaxEvalScripts; ++i) {
    eval(std::format("return {}", i + 100));
    if (i % 100 == 0) eval("return 7");  // recently used
  }
  EXPECT_EQ(text({"EVALSHA", sha1_hex("return 7"), "0"}), "7");
  EXPECT_EQ(text({"EVALSHA", sha1_hex("return 100"), "0"}), "NOSCRIPT No matching script. Please use EVAL.");
  EXPECT_EQ(text({"EVALSHA", sha1_hex(std::format("return {}", ScriptEngine::kMaxEvalScripts + 99)), "0"}), "599");
  EXPECT_EQ(text({"EVALSHA", sha, "0", "kept"}), "kept!");
  EXPECT_EQ(text({"SCRIPT", "FLUSH"}), "OK");
  EXPECT_EQ(text({"EVALSHA", sha, "0"}), "NOSCRIPT No matching script. Please use EVAL.");
  EXPECT_EQ(text({"SCRIPT", "KILL"}), "NOTBUSY No scripts in execution right now.");
  EXPECT_EQ(text({"EVAL", "return 1", "2", "k"}), "ERR Number of keys can't be greater than number of args");
  EXPECT_EQ(text({"EVAL", "return 1", "-1"}), "ERR Number of keys can't be negative");
}


TEST_F(Scripts, TheSandboxHasRedisLibrariesOnly) {
  for (const char* name : {"io", "require", "loadstring", "load", "dofile", "loadfile", "debug", "jit", "package"}) {
    Value e = eval(std::format("return {}", name));
    EXPECT_TRUE(e.error) << name;
    EXPECT_NE(e.text.find("nonexistent global variable"), std::string::npos) << e.text;
  }
  EXPECT_EQ(eval("return os.execute == nil and os.getenv == nil and 'safe' or 'unsafe'").text, "safe");
  EXPECT_EQ(eval("return type(os.clock())").text, "number");
  // Globals cannot be created (local variables are fine).
  EXPECT_NE(eval("x = 1").text.find("Script attempted to create global variable 'x'"), std::string::npos);
  EXPECT_EQ(eval("local x = 5 return x").text, "5");
  // The libraries Redis gives scripts.
  EXPECT_EQ(eval("return string.upper('ab') .. table.concat({1, 2}, ',') .. math.floor(2.5)").text, "AB1,22");
  EXPECT_EQ(eval("return bit.band(12, 10)").text, "8");
  EXPECT_EQ(eval("return redis.sha1hex('')").text, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(eval("return cjson.encode({b = {1, 2.5, 'x', true}})").text, R"({"b":[1,2.5,"x",true]})");
  EXPECT_EQ(eval(R"(local t = cjson.decode('{"n": [10, 20], "s": "v"}') return t.n[2] + #t.s)").text, "21");
  EXPECT_EQ(eval("return cjson.decode('[1, null]')[2] == cjson.null and 'null' or 'other'").text, "null");
  EXPECT_TRUE(eval("return cjson.decode('{bad')").error);
  EXPECT_EQ(eval("return redis.REDIS_VERSION").text, "7.2.0");
  // The state is reused, but one script's tables do not leak into the next.
  eval("local t = {} t.x = 1");
  EXPECT_TRUE(eval("return t").error);
}

TEST_F(Scripts, NoScriptChangesWhatTheNextOneFinds) {
  // Every script (of every user) shares the interpreter: redis.call replaced
  // by one would have run in the next, with that one's permissions.
  for (const char* attempt :
       {"redis.call = function() return 'hijacked' end", "redis = {}", "string.rep = nil", "cjson.encode = nil",
        "rawset(redis, 'call', nil)", "rawset(_G, 'redis', {})", "setmetatable(redis, {})",
        "getmetatable('').__index = {}", "setmetatable(_G, {})", "_G.redis = {}", "getfenv(1).redis = {}",
        "table.insert = nil", "math.floor = nil"}) {
    EXPECT_TRUE(eval(attempt).error) << attempt;
  }
  EXPECT_EQ(eval("return redis.call('PING')").text, "PONG");
  EXPECT_EQ(eval("return ('ab'):rep(2) .. string.rep('c', 2)").text, "ababcc");
  EXPECT_EQ(eval("return getmetatable(redis) == false and getmetatable('') == false and 'hidden' or 'visible'").text,
            "hidden");
  // The scripts' own tables are theirs to change.
  EXPECT_EQ(eval("local t = {} rawset(t, 'a', 1) setmetatable(t, {}) return t.a").text, "1");
  EXPECT_EQ(eval("return KEYS[1] .. ARGV[1]", {"k"}, {"v"}).text, "kv");
}

TEST_F(Scripts, ReadOnlyScripts) {
  text({"SET", "k", "v"});
  EXPECT_EQ(text({"EVAL_RO", "return redis.call('GET', KEYS[1])", "1", "k"}), "v");
  EXPECT_EQ(text({"EVAL_RO", "return redis.call('SET', 'k', 'w')", "0"}).find(
                "ERR Write commands are not allowed from read-only scripts."),
            0u);
  // The no-writes flag in the shebang.
  EXPECT_NE(eval("#!lua flags=no-writes\nreturn redis.call('DEL', 'k')").text.find("not allowed from read-only"),
            std::string::npos);
  EXPECT_EQ(eval("#!lua flags=no-writes\nreturn redis.call('GET', 'k')").text, "v");
  EXPECT_TRUE(eval("#!lua flags=bogus\nreturn 1").text.starts_with("ERR Unexpected flag"));
  EXPECT_TRUE(eval("#!python\nreturn 1").text.starts_with("ERR Could not find engine"));
  EXPECT_EQ(text({"GET", "k"}), "v");
}

TEST_F(Scripts, ScriptsCheckTheAclPerCommand) {
  EXPECT_EQ(text({"ACL", "SETUSER", "u", "on", "nopass", "+@scripting", "+get", "~*"}), "OK");
  EXPECT_EQ(text({"AUTH", "u", "x"}), "OK");
  EXPECT_EQ(eval("return redis.call('GET', 'k')").text, "");
  Value e = eval("return redis.call('SET', 'k', 'v')");
  EXPECT_TRUE(e.error);
  EXPECT_TRUE(e.text.starts_with("NOPERM")) << e.text;
}

TEST_F(Scripts, InsideMultiExec) {
  text({"MULTI"});
  EXPECT_EQ(text({"SET", "a", "1"}), "QUEUED");
  EXPECT_EQ(text({"EVAL", "return redis.call('INCR', KEYS[1])", "1", "a"}), "QUEUED");
  EXPECT_EQ(text({"EVAL", "redis.call('SET', 'other', 'x') return 'done'", "0"}), "QUEUED");
  Value r = run({"EXEC"});
  EXPECT_EQ(texts(r), (std::vector<std::string>{"OK", "2", "done"}));
  EXPECT_EQ(text({"GET", "other"}), "x");
}

TEST_F(Scripts, SetrespGivesRedisRESP3Types) {
  text({"HSET", "h", "f", "v"});
  EXPECT_EQ(eval("redis.setresp(3) local m = redis.call('HGETALL', 'h') return m.map.f").text, "v");
  EXPECT_EQ(eval("return redis.call('HGETALL', 'h')[2]").text, "v");  // RESP2 again in the next script
  EXPECT_EQ(eval("redis.setresp(3) return redis.call('GET', 'missing') == nil and 'nil' or 'not nil'").text, "nil");
  EXPECT_TRUE(eval("redis.setresp(4)").error);
}

TEST_F(Scripts, Functions) {
  std::string lib =
      "#!lua name=mylib\n"
      "local function incrby(keys, args) return redis.call('INCRBY', keys[1], args[1]) end\n"
      "redis.register_function('my_incrby', incrby)\n"
      "redis.register_function{function_name = 'my_get', callback = function(keys) return redis.call('GET', keys[1]) "
      "end, flags = {'no-writes'}}\n";
  EXPECT_EQ(text({"FUNCTION", "LOAD", lib}), "mylib");
  EXPECT_EQ(text({"FCALL", "my_incrby", "1", "c", "5"}), "5");
  EXPECT_EQ(text({"FCALL", "my_incrby", "1", "c", "2"}), "7");
  EXPECT_EQ(text({"FCALL_RO", "my_get", "1", "c"}), "7");
  EXPECT_EQ(text({"FCALL_RO", "my_incrby", "1", "c", "1"}),
            "ERR Can not execute a script with write flag using *_ro command.");
  EXPECT_EQ(text({"FCALL", "nope", "0"}), "ERR Function not found");
  EXPECT_EQ(text({"FUNCTION", "LOAD", lib}), "ERR Library 'mylib' already exists");
  EXPECT_EQ(text({"FUNCTION", "LOAD", "REPLACE", lib}), "mylib");
  // Function names are unique across libraries.
  EXPECT_EQ(text({"FUNCTION", "LOAD", "#!lua name=other\nredis.register_function('my_get', function() end)"}),
            "ERR Function my_get already exists");
  EXPECT_EQ(text({"FUNCTION", "LOAD", "return 1"}), "ERR Missing library metadata");
  EXPECT_TRUE(text({"FUNCTION", "LOAD", "#!lua name=empty\nlocal x = 1"}).starts_with("ERR No functions"));
  // While loading, redis.call is not there.
  EXPECT_NE(text({"FUNCTION", "LOAD", "#!lua name=bad\nredis.call('SET', 'x', 1)"}).find("Error registering"),
            std::string::npos);

  Value list = run({"FUNCTION", "LIST", "WITHCODE"});
  ASSERT_EQ(list.items.size(), 1u);
  std::vector<std::string> fields = texts(list.items[0]);
  ASSERT_GE(fields.size(), 8u);
  EXPECT_EQ(fields[0], "library_name");
  EXPECT_EQ(fields[1], "mylib");
  EXPECT_EQ(fields[3], "LUA");
  EXPECT_EQ(list.items[0].items[5].items.size(), 2u);  // two functions
  EXPECT_EQ(fields[7], lib);
  EXPECT_EQ(run({"FUNCTION", "LIST", "LIBRARYNAME", "zz*"}).items.size(), 0u);

  // The libraries are stored: a new engine (a restarted server) has them.
  a_ = ClientState{};
  ctx_.reset();
  ctx_ = std::make_unique<ServerContext>(*db_, config_, stats_, 0);
  EXPECT_EQ(text({"FCALL", "my_incrby", "1", "c", "3"}), "10");

  EXPECT_EQ(text({"FUNCTION", "DELETE", "mylib"}), "OK");
  EXPECT_EQ(text({"FUNCTION", "DELETE", "mylib"}), "ERR Library not found");
  EXPECT_EQ(text({"FCALL", "my_incrby", "1", "c", "3"}), "ERR Function not found");
  text({"FUNCTION", "LOAD", lib});
  EXPECT_EQ(text({"FUNCTION", "FLUSH"}), "OK");
  EXPECT_EQ(run({"FUNCTION", "LIST"}).items.size(), 0u);
}

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

 private:
  asio::io_context io_;
  tcp::socket socket_;
  std::string buf_;
};

class LiveScripts : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_lualive_" + std::to_string(std::random_device{}()));
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
    config_.io_threads = 4;
    config_.memcache_port = 0;
    config_.read_threads = 2;
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

// A read-modify-write in a script is atomic against other scripts and plain commands.
TEST_F(LiveScripts, ScriptsAreAtomic) {
  const std::string body =
      "local v = tonumber(redis.call('GET', KEYS[1]) or '0') redis.call('SET', KEYS[1], v + 1) return v + 1";
  constexpr int kThreads = 4, kEach = 300;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      Client c(port());
      for (int i = 0; i < kEach; ++i) {
        if (t == 0) {
          c.run({"INCR", "other"});  // plain commands in between, on other keys
        }
        c.run({"EVAL", body, "1", "counter"});
      }
    });
  }
  for (auto& t : threads) t.join();
  Client c(port());
  EXPECT_EQ(c.text({"GET", "counter"}), std::to_string(kThreads * kEach));
  EXPECT_EQ(c.text({"GET", "other"}), std::to_string(kEach));
}

// Readers (scripts and EXEC blocks) never see half of a script's writes,
// although the script lets other clients in while its writes commit.
TEST_F(LiveScripts, NoReaderSeesHalfAScript) {
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0}, reads{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 2; ++t) {
    threads.emplace_back([&, t] {
      Client c(port());
      for (int i = 0; !stop; ++i) {
        std::string v = std::to_string(t * 1000000 + i);
        c.run({"EVAL", "redis.call('SET', KEYS[1], ARGV[1]) redis.call('SET', KEYS[2], ARGV[1])", "2", "pa", "pb", v});
      }
    });
  }
  for (int t = 0; t < 2; ++t) {
    threads.emplace_back([&, t] {
      Client c(port());
      while (!stop) {
        Value r;
        if (t == 0) {
          r = c.run({"EVAL", "return {redis.call('GET', KEYS[1]), redis.call('GET', KEYS[2])}", "2", "pa", "pb"});
        } else {
          c.run({"MULTI"});
          c.run({"GET", "pa"});
          c.run({"GET", "pb"});
          r = c.run({"EXEC"});
        }
        if (r.items.size() == 2 && r.items[0].text != r.items[1].text) ++torn;
        ++reads;
      }
    });
  }
  std::this_thread::sleep_for(1500ms);
  stop = true;
  for (auto& t : threads) t.join();
  EXPECT_EQ(torn.load(), 0);
  EXPECT_GT(reads.load(), 100);
}

TEST_F(LiveScripts, ScriptKillStopsALoop) {
  Client runner(port()), killer(port());
  runner.send({"EVAL", "local i = 0 while true do i = i + 1 end", "0"});
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(killer.text({"FUNCTION", "STATS"}).size(), 0u);  // an array (map in RESP2), no text
  EXPECT_EQ(killer.text({"SCRIPT", "KILL"}), "OK");
  Value r = runner.read();
  EXPECT_TRUE(r.error);
  EXPECT_NE(r.text.find("Script killed by user with SCRIPT KILL"), std::string::npos) << r.text;
  EXPECT_EQ(runner.text({"PING"}), "PONG");
  // A script that catches the error (pcall) gets it again at once, at every level.
  runner.send({"EVAL",
               "while true do pcall(function() while true do pcall(function() while true do end end) end end) end",
               "0"});
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(killer.text({"SCRIPT", "KILL"}), "OK");
  r = runner.read();
  EXPECT_NE(r.text.find("Script killed by user with SCRIPT KILL"), std::string::npos) << r.text;
  // And the next script runs at full speed again.
  EXPECT_EQ(runner.text({"EVAL", "local i = 0 while i < 1e6 do i = i + 1 end return i", "0"}), "1000000");
  // A script that wrote cannot be killed.
  runner.send({"EVAL", "redis.call('SET', 'w', 1) local i = 0 while i < 3e8 do i = i + 1 end return 'end'", "0"});
  std::this_thread::sleep_for(200ms);
  EXPECT_TRUE(killer.text({"SCRIPT", "KILL"}).starts_with("UNKILLABLE"));
  // Other commands wait for the script (off the I/O threads), and see all its writes.
  Client reader(port());
  EXPECT_EQ(reader.text({"GET", "w"}), "1");
  EXPECT_EQ(runner.read().text, "end");
  EXPECT_EQ(killer.text({"SCRIPT", "KILL"}), "NOTBUSY No scripts in execution right now.");
}
