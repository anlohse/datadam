// ACL users and permissions, and CONFIG REWRITE.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "cache/object_cache.h"
#include "db/database.h"
#include "server/memcache_protocol.h"
#include "server/redis_commands.h"
#include "storage/rocks_storage.h"
#include "util/sha256.h"

using namespace datadam;
namespace fs = std::filesystem;

namespace {

class AclTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_acl_" + std::to_string(std::random_device{}()));
    StorageOptions so;
    so.dir = (dir_ / "data").string();
    so.fsync = FsyncPolicy::No;
    storage_ = RocksStorage::open(so);
    CacheOptions co;
    co.max_memory = 4 << 20;
    co.min_memory = 0;
    co.shards = 2;
    cache_ = std::make_unique<ObjectCache>(co);
    db_ = std::make_unique<Database>(*storage_, *cache_);
    make_context();
  }
  void TearDown() override {
    ctx_.reset();
    db_.reset();
    cache_.reset();
    storage_.reset();
    fs::remove_all(dir_);
  }
  void make_context() {
    ctx_.reset();
    ctx_ = std::make_unique<ServerContext>(*db_, config_, stats_, 0);
    admin_ = ClientState{};
    start_session(*ctx_, admin_);
  }

  std::string run(ClientState& client, std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, client.proto);
    execute_redis_command(*ctx_, client, views, reply);
    return out;
  }
  std::string run(std::vector<std::string> args) { return run(admin_, std::move(args)); }

  // A new connection logged in as user/password.
  ClientState login(const std::string& user, const std::string& password) {
    ClientState c;
    start_session(*ctx_, c);
    EXPECT_EQ(run(c, {"AUTH", user, password}), "+OK\r\n") << user;
    return c;
  }

  static bool noperm(const std::string& reply) { return reply.starts_with("-NOPERM"); }

  fs::path dir_;
  Config config_;
  ServerStats stats_;
  ClientState admin_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ServerContext> ctx_;
};

}  // namespace

TEST(Sha256Test, KnownVectors) {
  EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(sha256_hex(std::string(1'000'000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_F(AclTest, CommandsAndKeyPrefixes) {
  ASSERT_EQ(run({"ACL", "SETUSER", "alice", "on", ">pw", "~app:*", "+@read", "+set"}), "+OK\r\n");
  ClientState alice = login("alice", "pw");
  EXPECT_EQ(run(alice, {"ACL", "WHOAMI"}), "$5\r\nalice\r\n");
  EXPECT_EQ(run(alice, {"SET", "app:1", "v"}), "+OK\r\n");
  EXPECT_EQ(run(alice, {"GET", "app:1"}), "$1\r\nv\r\n");
  EXPECT_EQ(run(alice, {"GET", "other"}), "-NOPERM No permissions to access a key\r\n");
  EXPECT_EQ(run(alice, {"DEL", "app:1"}), "-NOPERM User alice has no permissions to run the 'del' command\r\n");
  EXPECT_TRUE(noperm(run(alice, {"MGET", "app:1", "secret"})));  // every key is checked
  EXPECT_EQ(run(alice, {"MGET", "app:1", "app:2"}).substr(0, 2), "*2");
  EXPECT_TRUE(noperm(run(alice, {"FLUSHALL"})));
  EXPECT_TRUE(noperm(run(alice, {"CONFIG", "GET", "port"})));
  EXPECT_TRUE(noperm(run(alice, {"PING"})));  // @connection is not granted either, as in Redis
}

TEST_F(AclTest, ReadOnlyAndWriteOnlyPatterns) {
  run({"SET", "ro:1", "v"});
  ASSERT_EQ(run({"ACL", "SETUSER", "bob", "on", ">pw", "%R~ro:*", "%W~wo:*", "+@all"}), "+OK\r\n");
  ClientState bob = login("bob", "pw");
  EXPECT_EQ(run(bob, {"GET", "ro:1"}), "$1\r\nv\r\n");
  EXPECT_TRUE(noperm(run(bob, {"SET", "ro:1", "x"})));
  EXPECT_EQ(run(bob, {"SET", "wo:1", "x"}), "+OK\r\n");
  EXPECT_TRUE(noperm(run(bob, {"GET", "wo:1"})));
  // SINTERSTORE writes its first key and reads the others.
  run({"SADD", "ro:s", "a"});
  EXPECT_EQ(run(bob, {"SINTERSTORE", "wo:dst", "ro:s"}), ":1\r\n");
  EXPECT_TRUE(noperm(run(bob, {"SINTERSTORE", "ro:dst", "ro:s"})));
  EXPECT_TRUE(noperm(run(bob, {"SINTERCARD", "2", "ro:s", "wo:dst"})));  // wo:dst is not readable
  EXPECT_EQ(run(bob, {"SINTERCARD", "1", "ro:s"}), ":1\r\n");
}

TEST_F(AclTest, SubcommandsAndCategories) {
  // Rules apply in order: -@dangerous also drops config|get, so it comes first.
  ASSERT_EQ(run({"ACL", "SETUSER", "carol", "on", "nopass", "allkeys", "+@all", "-@dangerous", "+info", "-config",
                 "+config|get"}),
            "+OK\r\n");
  ClientState carol = login("carol", "anything");  // nopass: any password
  EXPECT_EQ(run(carol, {"CONFIG", "GET", "port"}).substr(0, 2), "*2");
  EXPECT_EQ(run(carol, {"CONFIG", "SET", "log-level", "info"}),
            "-NOPERM User carol has no permissions to run the 'config|set' command\r\n");
  EXPECT_TRUE(noperm(run(carol, {"KEYS", "*"})));  // @dangerous
  EXPECT_EQ(run(carol, {"INFO", "server"}).substr(0, 1), "$");
  EXPECT_EQ(run(carol, {"SET", "k", "v"}), "+OK\r\n");
  EXPECT_NE(run({"ACL", "CAT", "read"}).find("hgetall"), std::string::npos);
  EXPECT_EQ(run({"ACL", "CAT", "nope"}).substr(0, 4), "-ERR");
}

TEST_F(AclTest, ScanKeysAndIndexesShowOnlyReadableKeys) {
  for (std::string p : {"a:", "b:"}) {
    for (int i = 0; i < 5; ++i) run({"HSET", p + std::to_string(i), "kind", "x"});
  }
  run({"IDX.CREATE", "kinds", "SCHEMA", "kind", "TAG"});
  for (int i = 0; i < 200 && db_->indexes().find("kinds")->state == IndexState::Building; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(run({"ACL", "SETUSER", "dave", "on", ">pw", "~a:*", "+@read", "+@keyspace", "+@index", "-@admin"}),
            "+OK\r\n");
  ClientState dave = login("dave", "pw");
  EXPECT_EQ(run(dave, {"SCAN", "0", "COUNT", "100"}).find("b:"), std::string::npos);
  EXPECT_NE(run(dave, {"SCAN", "0", "COUNT", "100"}).find("a:3"), std::string::npos);
  EXPECT_EQ(run(dave, {"KEYS", "*"}).substr(0, 2), "*5");
  EXPECT_EQ(run(dave, {"IDX.COUNT", "kinds", "kind", "=", "x"}), ":5\r\n");
  EXPECT_EQ(run(admin_, {"IDX.COUNT", "kinds", "kind", "=", "x"}), ":10\r\n");
  EXPECT_EQ(run(dave, {"IDX.QUERY", "kinds", "LIMIT", "0", "100", "NOCONTENT"}).find("b:"), std::string::npos);
  EXPECT_TRUE(noperm(run(dave, {"IDX.DROP", "kinds"})));  // @admin
}

TEST_F(AclTest, ChangesReachOpenConnections) {
  run({"ACL", "SETUSER", "eve", "on", ">pw", "~*", "+get"});
  ClientState eve = login("eve", "pw");
  EXPECT_EQ(run(eve, {"GET", "k"}), "$-1\r\n");
  run({"ACL", "SETUSER", "eve", "-get"});
  EXPECT_TRUE(noperm(run(eve, {"GET", "k"})));
  run({"ACL", "SETUSER", "eve", "off"});  // disabled: no new logins, open connections stay
  ClientState again;
  start_session(*ctx_, again);
  EXPECT_EQ(run(again, {"AUTH", "eve", "pw"}).substr(0, 10), "-WRONGPASS");
  EXPECT_EQ(run({"ACL", "DELUSER", "eve", "nobody"}), ":1\r\n");
  EXPECT_EQ(run(eve, {"PING"}).substr(0, 4), "-ERR");
  EXPECT_TRUE(eve.close_after_reply);
  EXPECT_EQ(run({"ACL", "DELUSER", "default"}).substr(0, 4), "-ERR");
}

TEST_F(AclTest, DescribeAndErrors) {
  ASSERT_EQ(run({"ACL", "SETUSER", "frank", "on", ">secret", "~x:*", "%R~y:*", "-@all", "+get", "+hset"}), "+OK\r\n");
  std::string hash = sha256_hex("secret");
  std::string line = std::format("user frank on #{} ~x:* %R~y:* resetchannels -@all +get +hset", hash);
  std::string list = run({"ACL", "LIST"});
  EXPECT_NE(list.find(line), std::string::npos) << list;
  EXPECT_NE(list.find("user default on nopass ~* &* +@all"), std::string::npos);
  std::string user = run({"ACL", "GETUSER", "frank"});
  EXPECT_NE(user.find(hash), std::string::npos);
  EXPECT_NE(user.find("$16\r\n-@all +get +hset\r\n"), std::string::npos) << user;
  EXPECT_EQ(run({"ACL", "GETUSER", "nobody"}), "$-1\r\n");
  EXPECT_EQ(run({"ACL", "USERS"}), "*2\r\n$7\r\ndefault\r\n$5\r\nfrank\r\n");
  // An invalid rule changes nothing.
  EXPECT_NE(run({"ACL", "SETUSER", "frank", "+set", "+nosuchcmd"}).find("Unknown command"), std::string::npos);
  EXPECT_NE(run({"ACL", "LIST"}).find(line), std::string::npos);
  EXPECT_NE(run({"ACL", "SETUSER", "frank", "#short"}).find("64 characters"), std::string::npos);
  EXPECT_NE(run({"ACL", "SETUSER", "frank", "<notthere"}).find("does not exist"), std::string::npos);
  EXPECT_NE(run({"ACL", "SETUSER", "frank", "%X~a"}).find("Syntax error"), std::string::npos);
  // requirepass is the default user's password.
  EXPECT_EQ(run({"CONFIG", "SET", "requirepass", "pw"}), "+OK\r\n");
  EXPECT_NE(run({"ACL", "LIST"}).find("user default on #" + sha256_hex("pw")), std::string::npos);
  EXPECT_TRUE(ctx_->auth_required);
}

TEST_F(AclTest, MemcacheUsesThePermissionsOfItsUser) {
  run({"ACL", "SETUSER", "mc", "on", ">pw", "~app:*", "+@read", "+set"});
  run({"CONFIG", "SET", "requirepass", "x"});
  MemcacheSession session(*ctx_);
  auto send = [&](std::string_view req) {
    std::string out;
    size_t pos = 0;
    session.process(req, pos, out);
    return out;
  };
  EXPECT_EQ(send("auth mc pw\r\n"), "OK\r\n");
  EXPECT_EQ(send("set app:1 0 0 1\r\nv\r\n"), "STORED\r\n");
  EXPECT_EQ(send("get app:1\r\n"), "VALUE app:1 0 1\r\nv\r\nEND\r\n");
  EXPECT_EQ(send("get other\r\n"), "CLIENT_ERROR NOPERM no permissions to access a key\r\n");
  EXPECT_EQ(send("set other 0 0 1\r\nv\r\n"), "CLIENT_ERROR NOPERM no permissions to access a key\r\n");
  EXPECT_NE(send("delete app:1\r\n").find("NOPERM"), std::string::npos);
}

TEST_F(AclTest, ConfigRewriteKeepsTheFileAndAddsWhatChanged) {
  EXPECT_EQ(run({"CONFIG", "REWRITE"}), "-ERR The server is running without a config file\r\n");

  fs::path conf = dir_ / "datadam.conf";
  fs::create_directories(dir_);
  {
    std::ofstream f(conf);
    f << "# my settings\nport 7000\ncache-max-idle 5m   \n\n# cache\ncache-shards 16\n";
  }
  config_ = Config{};
  ASSERT_EQ(config_.load_file(conf.string()), "");
  config_.config_file = conf.string();
  make_context();
  ASSERT_EQ(run({"CONFIG", "SET", "cache-max-idle", "1m", "fsync", "always"}), "+OK\r\n");
  ASSERT_EQ(run({"ACL", "SETUSER", "alice", "on", ">pw", "~app:*", "+@read"}), "+OK\r\n");
  ASSERT_EQ(run({"CONFIG", "REWRITE"}), "+OK\r\n");

  std::stringstream text;
  text << std::ifstream(conf).rdbuf();
  std::string s = text.str();
  EXPECT_TRUE(s.starts_with("# my settings\nport 7000\ncache-max-idle 1m\n\n# cache\ncache-shards 16\n"))
      << s;
  EXPECT_NE(s.find("# Generated by CONFIG REWRITE\n"), std::string::npos);
  EXPECT_NE(s.find("\nfsync always\n"), std::string::npos);
  EXPECT_NE(s.find("\nuser alice on #" + sha256_hex("pw") + " ~app:* resetchannels -@all +@read\n"), std::string::npos)
      << s;
  EXPECT_EQ(s.find("user default"), std::string::npos);  // unchanged default user is not written

  // A server started from the rewritten file has the same settings and users.
  Config reloaded;
  ASSERT_EQ(reloaded.load_file(conf.string()), "");
  EXPECT_EQ(reloaded.cache_max_idle.count(), 60'000);
  config_ = reloaded;
  make_context();
  ClientState alice = login("alice", "pw");
  EXPECT_TRUE(noperm(run(alice, {"GET", "other"})));
  EXPECT_EQ(run(alice, {"GET", "app:1"}), "$-1\r\n");

  // Rewriting again does not duplicate anything.
  config_.config_file = conf.string();
  make_context();
  ASSERT_EQ(run({"CONFIG", "REWRITE"}), "+OK\r\n");
  std::stringstream again;
  again << std::ifstream(conf).rdbuf();
  EXPECT_EQ(again.str(), s);
}

TEST_F(AclTest, ConfigRewriteReplacesSlaveofLines) {
  fs::path conf = dir_ / "datadam.conf";
  fs::create_directories(dir_);
  auto rewrite = [&](std::string_view replicaof) {
    {
      std::ofstream f(conf, std::ios::trunc);
      f << "port 7000\nslaveof 10.0.0.1 6379\nlog-level info\n";
    }
    config_ = Config{};
    EXPECT_EQ(config_.load_file(conf.string()), "");
    config_.config_file = conf.string();
    make_context();
    ctx_->set_replicaof(std::string(replicaof));  // what REPLICAOF does
    EXPECT_EQ(run({"CONFIG", "REWRITE"}), "+OK\r\n");
    std::stringstream text;
    text << std::ifstream(conf).rdbuf();
    return text.str();
  };
  // Following another primary: the line gets the new one, under the current name.
  EXPECT_EQ(rewrite("10.0.0.2 6380"), "port 7000\nreplicaof 10.0.0.2 6380\nlog-level info\n");
  // Promoted (REPLICAOF NO ONE): the line goes, so a restart does not follow the old primary.
  EXPECT_EQ(rewrite(""), "port 7000\nlog-level info\n");
}

TEST_F(AclTest, AclLogRecordsRefusalsAndFailedLogins) {
  run({"ACL", "SETUSER", "bob", "on", ">pw1", "+get"});
  ClientState c;
  start_session(*ctx_, c);
  EXPECT_EQ(run(c, {"AUTH", "bob", "wrong"}).substr(0, 10), "-WRONGPASS");
  EXPECT_EQ(run(c, {"AUTH", "bob", "pw1"}), "+OK\r\n");
  EXPECT_TRUE(noperm(run(c, {"SET", "k", "v"})));
  EXPECT_TRUE(noperm(run(c, {"GET", "secret"})));  // bob has no key patterns
  EXPECT_TRUE(noperm(run(c, {"GET", "secret"})));  // a repeat: the same entry, count 2

  std::string log = run({"ACL", "LOG"});
  ASSERT_EQ(log.substr(0, 4), "*3\r\n") << log;
  size_t key = log.find("$3\r\nkey\r\n"), command = log.find("$7\r\ncommand\r\n"), auth = log.find("$4\r\nauth\r\n");
  ASSERT_NE(key, std::string::npos);
  ASSERT_NE(command, std::string::npos);
  ASSERT_NE(auth, std::string::npos);
  EXPECT_LT(key, command);  // newest first
  EXPECT_LT(command, auth);
  EXPECT_NE(log.find("$5\r\ncount\r\n:2\r\n"), std::string::npos);
  EXPECT_NE(log.find("$6\r\nsecret\r\n"), std::string::npos);
  EXPECT_NE(log.find("$3\r\nset\r\n"), std::string::npos);
  EXPECT_NE(log.find("user=bob"), std::string::npos);  // client-info
  EXPECT_EQ(run({"ACL", "LOG", "1"}).substr(0, 4), "*1\r\n");
  EXPECT_EQ(run({"ACL", "LOG", "RESET"}), "+OK\r\n");
  EXPECT_EQ(run({"ACL", "LOG"}), "*0\r\n");
  EXPECT_EQ(run({"CONFIG", "SET", "acllog-max-len", "0"}), "+OK\r\n");
  run(c, {"SET", "k", "v"});
  EXPECT_EQ(run({"ACL", "LOG"}), "*0\r\n");
}
