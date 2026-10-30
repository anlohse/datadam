#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace datadam {

enum class FsyncPolicy { Always, EverySec, No };

// Experimental, for measuring the write path. Only Wal is safe for real data.
//   Wal         - every write goes to the RocksDB WAL (write syscall) before the reply
//   WalBuffered - the WAL is buffered in memory and written by the maintenance thread
//                 every second (a process crash loses up to ~1s of writes)
//   NoWal       - no WAL: only memtable flushes persist data (a crash loses the memtables)
//   Memory      - nothing is written to RocksDB at all (benchmark upper bound only)
enum class WritePath { Wal, WalBuffered, NoWal, Memory };

struct Config {
  // Network
  std::string bind = "0.0.0.0";
  uint16_t port = 6379;           // RESP (Redis protocol)
  uint16_t memcache_port = 11211; // memcache text protocol; 0 disables
  // HTTP for monitoring (GET /metrics in Prometheus text, /health, /ready); 0 disables.
  uint16_t metrics_port = 0;
  // TLS for the Redis protocol (0 disables). "port 0" then leaves only the TLS port.
  uint16_t tls_port = 0;
  std::string tls_cert_file;          // server certificate chain (PEM)
  std::string tls_key_file;           // its private key (PEM)
  std::string tls_ca_cert_file;       // CAs that sign client certificates
  std::string tls_auth_clients = "yes";  // yes: a client certificate is required; optional; no
  std::string tls_auth_clients_user = "off";  // cn: a client certificate logs in as the ACL user named by its CN
  std::string tls_protocols = "TLSv1.2,TLSv1.3";
  std::string tls_ciphers;            // TLS 1.2 cipher list (OpenSSL syntax); empty: OpenSSL's default
  std::string tls_ciphersuites;       // TLS 1.3 cipher suites; empty: OpenSSL's default
  // Replication over TLS: a replica connects to its primary's TLS port, checks
  // its certificate against tls-ca-cert-file (or the system's CAs) and the host
  // name, and presents tls-client-cert-file (default: tls-cert-file) if it has one.
  bool tls_replication = false;
  // Protected mode (as in Redis): while the default user needs no password,
  // only loopback clients may connect (TLS clients with a verified
  // certificate too).
  bool protected_mode = true;
  std::string tls_client_cert_file;
  std::string tls_client_key_file;
  // 0: auto (io_thread_count). Cloud phase 4, in memory with 800 connections:
  // on 16 vCPUs 8 I/O threads did 20% more mixed reads and writes than 16, which
  // left too little CPU to RocksDB and the read pool; on 4 and 8 vCPUs one
  // thread per vCPU did best.
  unsigned io_threads = 0;
  // Threads that load keys missing from the cache for plain reads (GET, MGET...),
  // so a disk read never holds an I/O thread; 0: the I/O thread reads the disk.
  unsigned read_threads = 64;
  // Most keys one read thread loads with a single MultiGet when keys queue up
  // (1: one Get per key). Measured no faster on random keys, so off by default.
  unsigned read_batch = 1;
  // Whether such a read is first tried on the I/O thread from the OS page
  // cache (Linux, RWF_NOWAIT): only a block that is on disk goes to the pool.
  bool page_cache_reads = true;

  // Persistence
  std::string dir = "./data";
  FsyncPolicy fsync = FsyncPolicy::EverySec;
  WritePath write_path = WritePath::Wal;
  bool write_batching = true;  // one RocksDB write per pipelined read (group commit)

  // Object cache (memory pool)
  uint64_t cache_max_memory = 512ULL << 20;   // hard ceiling of the pool
  uint64_t cache_min_memory = 64ULL << 20;    // idle eviction never shrinks below this
  uint64_t cache_max_object_size = 4096;      // key + value; larger objects are served from disk
  std::chrono::milliseconds cache_max_idle{std::chrono::minutes(5)};  // 0 disables
  std::chrono::milliseconds cache_max_age{0};                         // 0 disables
  std::chrono::milliseconds cache_maintenance_interval{1000};
  unsigned cache_shards = 16;
  bool cache_on_write = true;  // writes populate the cache (write-through)
  bool cache_admission = true; // when full, admit a key only on its second access (TinyLFU doorkeeper)

  // RocksDB
  uint64_t rocksdb_block_cache = 64ULL << 20;
  uint64_t rocksdb_write_buffer = 64ULL << 20;
  int rocksdb_background_jobs = 4;
  // Change feed (CDC.READ): how long, and how many bytes of, WAL to keep
  // after it is no longer needed for recovery. 0 turns the feed off.
  std::chrono::milliseconds wal_retention{0};
  uint64_t wal_retention_size = 1ULL << 30;
  // How far past wal_retention_size the kept WAL may grow while a connected
  // replica still needs it (a long full sync under heavy writes).
  uint64_t wal_retention_hold = 8ULL << 30;

  std::string log_level = "info";

  // Security: when set, Redis clients must AUTH with it (memcache clients send "auth <password>").
  // It is the password of the "default" user.
  std::string requirepass;
  // Replication: "host port" of the primary to follow (empty: this is a
  // primary), and the user and password to log in to it with.
  std::string replicaof;
  std::string masteruser;
  std::string masterauth;
  // Redis Sentinel promotes the replica with the lowest non-zero priority (0: never).
  unsigned replica_priority = 100;
  // ACL users, one "user" line each: "<name> <rule> ..." (defines the user from scratch).
  std::vector<std::string> users;
  unsigned acllog_max_len = 128;  // entries kept by ACL LOG

  // BGSAVE writes a checkpoint of the database in backup_dir/<timestamp> (empty:
  // BGSAVE only flushes, like SAVE) and keeps the newest backup_keep (0: all).
  std::string backup_dir;
  unsigned backup_keep = 7;
  // SLOWLOG: commands that take at least this many microseconds (-1: none, 0: all).
  int64_t slowlog_log_slower_than = 10000;
  unsigned slowlog_max_len = 128;

  // The file the settings came from (CONFIG REWRITE writes it back); empty if none.
  std::string config_file;

  // Each returns an empty string on success or an error message.
  std::string set(std::string_view key, std::string_view value);
  std::string load_file(const std::string& path);
  // Usage: datadam [config-file] [--key value]...
  std::string parse_args(int argc, char** argv);
  std::string validate();

  // (name, value) pairs in config-file syntax, for CONFIG GET and INFO.
  std::vector<std::pair<std::string, std::string>> dump() const;

  // The I/O threads to run: io-threads, or with auto one per CPU up to 8,
  // then half the CPUs.
  unsigned io_thread_count() const;
};

// io-threads auto for a machine with `cpus` CPUs.
unsigned auto_io_threads(unsigned cpus);

std::string_view fsync_policy_name(FsyncPolicy p);
std::string_view write_path_name(WritePath p);

}  // namespace datadam
