#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "util/hash.h"
#include "util/latency_histogram.h"

namespace datadam {

class Database;

// Threads that load keys missing from the object cache, so that a read never
// holds an I/O thread while the disk works (Database::load_into_cache). A key
// requested again while its load is queued or running is read once: every
// requester is told when it is done (single flight). When more keys are queued
// than there are threads, a thread takes several (up to max_batch) and reads
// them with one MultiGet.
class ReadPool {
 public:
  // Called on a pool thread with whether the key is now cached.
  using Done = std::function<void(bool cached)>;

  ReadPool(Database& db, unsigned threads, unsigned max_batch = 1);
  ~ReadPool();  // finishes the loads under way and drops the queued ones
  ReadPool(const ReadPool&) = delete;
  ReadPool& operator=(const ReadPool&) = delete;

  // Loads `key` and then calls done (if any). Without done it is a prefetch:
  // nobody waits, the key is just warmed for a command further down a pipeline.
  void load(std::string_view key, Done done);

  // Time from a load request to its completion.
  const LatencyHistogram& latency() const { return latency_; }
  void reset_latency() { latency_.reset(); }
  uint64_t loads() const;    // keys loaded
  uint64_t batches() const;  // loads made (one key, or several with one MultiGet)
  uint64_t joined() const;   // requests that joined a load already under way
  size_t queued() const;

 private:
  struct Flight {
    std::vector<Done> waiters;
    std::chrono::steady_clock::time_point since;
  };
  struct KeyHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return static_cast<size_t>(hash_bytes(s)); }
  };

  void run();

  Database& db_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  std::unordered_map<std::string, Flight, KeyHash, std::equal_to<>> flights_;  // queued or loading
  unsigned max_batch_;
  uint64_t loads_ = 0;
  uint64_t batches_ = 0;
  uint64_t joined_ = 0;
  bool stop_ = false;
  LatencyHistogram latency_;
  std::vector<std::thread> threads_;  // last: they start once the rest exists
};

}  // namespace datadam
