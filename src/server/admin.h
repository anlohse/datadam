#pragma once

// Server administration state: backups (SAVE / BGSAVE / BACKUP / LASTSAVE),
// the slow log (SLOWLOG), the connections (CLIENT LIST / KILL) and MONITOR.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "storage/rocks_storage.h"

namespace datadam {

class Subscriber;

// Consistent copies of the database (RocksDB checkpoints).
// `path` as a directory inside `dir` (relative paths are taken from there),
// or nothing if it would be outside it (.., another root, a symbolic link out).
std::optional<std::string> backup_path_inside(const std::string& dir, const std::string& path);

class Backups {
 public:
  struct Status {
    bool in_progress = false;
    int64_t last_save_s = 0;  // unix seconds of the last successful save
    bool last_ok = true;
    std::string last_path;    // where the last backup went ("" for SAVE)
    std::string last_error;
    int64_t last_duration_ms = 0;
  };

  Backups(RocksStorage& storage, int64_t start_time_s);
  ~Backups();  // waits for a background backup
  Backups(const Backups&) = delete;
  Backups& operator=(const Backups&) = delete;

  // A checkpoint at `path`, which must not exist (its parent is created).
  // "" on success, or an error message.
  std::string backup(const std::string& path);
  // BGSAVE: a checkpoint in dir/<UTC yyyymmdd-hhmmss>, in the background;
  // afterwards only the newest `keep` of those are kept (0: all). False if
  // one is still running.
  bool start(const std::string& dir, unsigned keep);
  // SAVE (a flush): counts as a save.
  void saved();
  Status status() const;

 private:
  // The checkpoint itself; records the outcome (and, for BGSAVE, that it is
  // no longer running only once `background` work is over).
  std::string checkpoint(const std::string& path, bool background);
  void finish(bool ok, std::string path, std::string error, int64_t duration_ms, bool background);

  RocksStorage& storage_;
  mutable std::mutex mu_;
  Status status_;
  std::thread worker_;
};

// SLOWLOG: commands that took at least slowlog-log-slower-than microseconds.
class SlowLog {
 public:
  struct Entry {
    uint64_t id;
    int64_t time_s;
    int64_t duration_us;
    std::vector<std::string> args;
    std::string addr, name;
  };

  // slower_than_us < 0: off; 0: every command.
  void configure(int64_t slower_than_us, size_t max_len);
  bool enabled() const { return slower_than_.load(std::memory_order_relaxed) >= 0; }
  bool wants(int64_t us) const {
    int64_t t = slower_than_.load(std::memory_order_relaxed);
    return t >= 0 && us >= t;
  }
  void add(std::span<const std::string_view> args, int64_t us, std::string addr, std::string name);
  std::vector<Entry> get(size_t count) const;  // newest first
  size_t len() const;
  void reset();

 private:
  std::atomic<int64_t> slower_than_{10000};
  std::atomic<size_t> max_len_{128};
  mutable std::mutex mu_;
  std::deque<Entry> entries_;  // newest first
  uint64_t next_id_ = 0;
};

// The arguments a log may show: passwords are replaced by "(redacted)", and
// (as in Redis) at most 32 arguments of at most 128 bytes are kept.
std::vector<std::string> loggable_args(std::span<const std::string_view> args);

// What CLIENT LIST shows of a connection. Its own thread writes it; others read.
struct ClientEntry {
  uint64_t id = 0;
  int64_t created_ms = 0;  // steady clock
  std::atomic<int64_t> last_ms{0};
  std::atomic<const std::string*> last_cmd{nullptr};  // a key of the command table
  std::atomic<uint32_t> subscriptions{0}, patterns{0}, shard_subscriptions{0};
  std::atomic<bool> blocked{false}, monitor{false}, pubsub{false}, replica{false};
  // Changed rarely (AUTH, HELLO, CLIENT SETNAME/SETINFO), under mu.
  mutable std::mutex mu;
  std::string addr, name, user, lib_name, lib_ver;
  int proto = 2;
  bool tls = false;
  // Closes the connection (from any thread).
  std::function<void()> kill;

  std::string describe() const;  // a CLIENT LIST line, without the newline
};

class ClientRegistry {
 public:
  std::shared_ptr<ClientEntry> add(uint64_t id);
  void remove(uint64_t id);
  std::vector<std::shared_ptr<ClientEntry>> all() const;  // by id
  size_t size() const;
  // The server goes away: forgets every connection and their kill callbacks
  // (they hold executors of its io_context).
  void clear();

 private:
  mutable std::mutex mu_;
  std::map<uint64_t, std::shared_ptr<ClientEntry>> entries_;
};

// MONITOR: every command, as the server runs it, to the monitoring connections.
class Monitors {
 public:
  void add(uint64_t client_id, std::shared_ptr<Subscriber> s);
  bool any() const { return count_.load(std::memory_order_relaxed) > 0; }
  // "+<unix seconds>.<microseconds> [0 <addr>] "arg" ..." to every monitor but the sender.
  void feed(uint64_t from_id, std::string_view addr, std::span<const std::string_view> args);

 private:
  struct Watcher {
    uint64_t id;
    std::weak_ptr<Subscriber> sub;
  };
  std::mutex mu_;
  std::vector<Watcher> watchers_;
  std::atomic<size_t> count_{0};
};

}  // namespace datadam
