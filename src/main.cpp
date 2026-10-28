#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "cache/object_cache.h"
#include "config/config.h"
#include "db/database.h"
#include "server/server.h"
#include "storage/rocks_storage.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/strings.h"

using namespace datadam;

namespace {

// Background work: cache idle/age eviction and, once per second, a WAL fsync
// (fsync=everysec) or a write of the buffered WAL (experimental-write-path
// wal-buffered), and the wal-retention-size limit of the change feed.
class Maintenance {
 public:
  Maintenance(const Config& cfg, ObjectCache& cache, RocksStorage& storage)
      : cfg_(cfg), cache_(cache), storage_(storage), thread_([this] { loop(); }) {}

  ~Maintenance() {
    {
      std::lock_guard lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

 private:
  void loop() {
    auto tick = std::min<std::chrono::milliseconds>(cfg_.cache_maintenance_interval, std::chrono::seconds(1));
    int64_t next_cache = steady_time_ms();
    int64_t next_sync = steady_time_ms() + 1000;
    int64_t next_trim = steady_time_ms() + 1000;
    std::unique_lock lock(mu_);
    while (!cv_.wait_for(lock, tick, [this] { return stop_; })) {
      lock.unlock();
      int64_t now = steady_time_ms();
      try {
        if (now >= next_cache) {
          size_t n = cache_.run_maintenance();
          if (n > 0) log_debug("cache maintenance evicted {} items", n);
          next_cache = now + cfg_.cache_maintenance_interval.count();
        }
        bool periodic_sync =
            storage_.fsync_policy() == FsyncPolicy::EverySec || cfg_.write_path == WritePath::WalBuffered;
        if (periodic_sync && now >= next_sync) {
          storage_.sync_wal();
          next_sync = now + 1000;
        }
        if (storage_.wal_retained() && now >= next_trim) {
          if (size_t n = storage_.trim_wal()) log_debug("deleted {} kept WAL files (wal-retention-size)", n);
          next_trim = now + 1000;
        }
      } catch (const std::exception& e) {
        log_error("maintenance: {}", e.what());
      }
      lock.lock();
    }
  }

  const Config& cfg_;
  ObjectCache& cache_;
  RocksStorage& storage_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;  // last: starts after the other members exist
};

void usage() {
  std::fprintf(stderr,
               "usage: datadam [config-file] [--option value]...\n"
               "  e.g. datadam datadam.conf --port 6380 --cache-max-memory 1gb\n");
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (auto err = cfg.parse_args(argc, argv); !err.empty()) {
    std::fprintf(stderr, "datadam: %s\n", err.c_str());
    usage();
    return 1;
  }
  LogLevel level;
  if (parse_log_level(cfg.log_level, &level)) set_log_level(level);

  try {
    StorageOptions so;
    so.dir = cfg.dir;
    so.fsync = cfg.fsync;
    so.write_path = cfg.write_path;
    so.block_cache = cfg.rocksdb_block_cache;
    so.write_buffer = cfg.rocksdb_write_buffer;
    so.background_jobs = cfg.rocksdb_background_jobs;
    so.wal_retention_s = static_cast<uint64_t>(cfg.wal_retention.count() / 1000);
    so.wal_retention_bytes = cfg.wal_retention_size;
    so.wal_hold_bytes = cfg.wal_retention_hold;
    auto storage = RocksStorage::open(so);

    CacheOptions co;
    co.max_memory = cfg.cache_max_memory;
    co.min_memory = cfg.cache_min_memory;
    co.max_object_size = cfg.cache_max_object_size;
    co.max_idle_ms = cfg.cache_max_idle.count();
    co.max_age_ms = cfg.cache_max_age.count();
    co.shards = cfg.cache_shards;
    co.admission = cfg.cache_admission;
    ObjectCache cache(co);
    if (!cache.enabled()) log_warn("object cache disabled (cache-max-memory smaller than one page)");

    // A replica writes only what its primary sends; the primary builds the indexes.
    bool replica = !cfg.replicaof.empty();
    Database db(*storage, cache, cfg.cache_on_write, !replica);
    db.set_read_only(replica);
    ServerStats stats;
    ServerContext ctx(db, cfg, stats, unix_time_ms());

    Maintenance maintenance(cfg, cache, *storage);
    Server server(ctx);
    server.start();
    auto port_or_off = [](uint16_t p) { return p ? std::to_string(p) : std::string("off"); };
    log_info("datadam {} | redis {}:{} | tls {} | memcache {} | io threads {} | data dir {} | fsync {}", DATADAM_VERSION,
             cfg.bind, port_or_off(cfg.port), port_or_off(cfg.tls_port), port_or_off(cfg.memcache_port),
             cfg.io_thread_count(), cfg.dir, fsync_policy_name(cfg.fsync));
    if (cfg.tls_port != 0) {
      log_info("tls: certificate {} | client certificates {} | login by certificate CN {}", cfg.tls_cert_file,
               cfg.tls_auth_clients, cfg.tls_auth_clients_user);
    }
    if (cfg.write_path != WritePath::Wal) {
      log_warn("experimental-write-path {}: writes are NOT durable as configured", write_path_name(cfg.write_path));
    }
    log_info("cache: max {} min {} objects <= {} | page {} | {} size classes | idle {}ms age {}ms",
             format_size(co.max_memory), format_size(co.min_memory), format_size(co.max_object_size),
             format_size(cache.page_size()), cache.classes().count(), co.max_idle_ms, co.max_age_ms);
    server.run();
    log_info("stopped");
  } catch (const std::exception& e) {
    log_error("fatal: {}", e.what());
    return 1;
  }
  return 0;
}
