#include "server/read_pool.h"

#include <algorithm>

#include "db/database.h"
#include "util/log.h"

namespace datadam {

ReadPool::ReadPool(Database& db, unsigned threads, unsigned max_batch)
    : db_(db), max_batch_(max_batch < 1 ? 1 : max_batch) {
  for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

ReadPool::~ReadPool() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) t.join();
  // Loads still queued are dropped; whoever waits is told the key is not cached.
  for (auto& [key, flight] : flights_) {
    for (auto& done : flight.waiters) done(false);
  }
}

void ReadPool::load(std::string_view key, Done done) {
  {
    std::lock_guard lock(mu_);
    auto it = flights_.find(key);
    if (it != flights_.end()) {
      ++joined_;
      if (done) it->second.waiters.push_back(std::move(done));
      return;
    }
    Flight& f = flights_.try_emplace(std::string(key)).first->second;
    f.since = std::chrono::steady_clock::now();
    if (done) f.waiters.push_back(std::move(done));
    queue_.emplace_back(key);
  }
  cv_.notify_one();
}

void ReadPool::run() {
  for (;;) {
    std::vector<std::string> keys;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      // Batch only the backlog: while there are threads for every queued key,
      // separate reads run in parallel, which one MultiGet would serialize.
      size_t n = std::clamp<size_t>(queue_.size() / threads_.size(), 1, max_batch_);
      for (size_t i = 0; i < n; ++i) {
        keys.push_back(std::move(queue_.front()));
        queue_.pop_front();
      }
      loads_ += keys.size();
      ++batches_;
    }
    std::vector<bool> cached(keys.size(), false);
    try {
      if (keys.size() == 1) cached[0] = db_.load_into_cache(keys[0]);
      else cached = db_.load_many_into_cache(keys);
    } catch (const std::exception& e) {
      // The waiters run the command again without deferring; it reports the error.
      log_warn("read pool: loading keys failed: {}", e.what());
    }
    for (size_t i = 0; i < keys.size(); ++i) {
      Flight flight;
      {
        std::lock_guard lock(mu_);
        auto it = flights_.find(keys[i]);
        flight = std::move(it->second);
        flights_.erase(it);
      }
      auto d = std::chrono::steady_clock::now() - flight.since;
      latency_.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count()));
      for (auto& done : flight.waiters) done(cached[i]);
    }
  }
}

uint64_t ReadPool::loads() const {
  std::lock_guard lock(mu_);
  return loads_;
}

uint64_t ReadPool::batches() const {
  std::lock_guard lock(mu_);
  return batches_;
}

uint64_t ReadPool::joined() const {
  std::lock_guard lock(mu_);
  return joined_;
}

size_t ReadPool::queued() const {
  std::lock_guard lock(mu_);
  return queue_.size();
}

}  // namespace datadam
