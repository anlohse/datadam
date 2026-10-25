// libFuzzer target: a stream of RESP requests run as commands against a real
// database (RocksDB in a temporary directory, the object cache, indexes, Lua),
// as a client would send them. Every command handler is reachable, with any
// arguments. Left out: commands that act outside the process (replication,
// failover, backups, shutdown) or that would lock the next inputs out (ACL,
// CONFIG).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cache/object_cache.h"
#include "db/database.h"
#include "server/redis_commands.h"
#include "server/resp.h"
#include "storage/rocks_storage.h"
#include "util/strings.h"

using namespace datadam;

namespace {

struct World {
  Config config;
  ServerStats stats;
  std::unique_ptr<RocksStorage> storage;
  std::unique_ptr<ObjectCache> cache;
  std::unique_ptr<Database> db;
  std::unique_ptr<ServerContext> ctx;
  uint64_t inputs = 0;

  World() {
    auto dir = std::filesystem::temp_directory_path() / ("datadam_fuzz_" + std::to_string(std::random_device{}()));
    StorageOptions so;
    so.dir = dir.string();
    so.fsync = FsyncPolicy::No;
    so.write_buffer = 4 << 20;
    storage = RocksStorage::open(so);
    CacheOptions co;
    co.max_memory = 8 << 20;
    co.min_memory = 0;
    co.shards = 2;
    cache = std::make_unique<ObjectCache>(co);
    db = std::make_unique<Database>(*storage, *cache);
    ctx = std::make_unique<ServerContext>(*db, config, stats, 0);
  }
};

World& world() {
  static World* w = new World();  // never destroyed: the fuzzer exits without unwinding
  return *w;
}

bool excluded(const std::string& name) {
  static const std::set<std::string, std::less<>> names = {
      "shutdown", "replicaof", "slaveof", "failover", "sync", "psync", "replsync", "replconf",
      "save",     "bgsave",    "backup",  "config",   "acl",  "debug",
  };
  return names.contains(name);
}

void run(World& w, ClientState& client, std::vector<std::string_view> args) {
  std::string out;
  Reply reply(out, client.proto);
  execute_redis_command(*w.ctx, client, args, reply);
  client.retry_later = false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  World& w = world();
  // Keep the database small: start over now and then.
  if (++w.inputs % 2000 == 0) {
    ClientState admin;
    start_session(*w.ctx, admin);
    run(w, admin, {"FLUSHALL"});
    run(w, admin, {"SCRIPT", "FLUSH"});
    run(w, admin, {"FUNCTION", "FLUSH"});
  }

  std::string_view buf(reinterpret_cast<const char*>(data), size);
  ClientState client;
  start_session(*w.ctx, client);
  size_t pos = 0;
  std::vector<std::string_view> args;
  std::string error;
  for (int i = 0; i < 32 && pos < buf.size(); ++i) {
    if (RespParser::parse(buf, pos, args, error) != RespParser::Status::Ok) break;
    if (args.empty() || excluded(to_lower(args[0]))) continue;
    run(w, client, args);
    if (client.close_after_reply) break;
  }
  return 0;
}
