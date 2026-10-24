#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/hash.h"

namespace datadam {

// Who wants to know when a key changes: WATCH (a flag that EXEC checks) and
// blocked commands (BLPOP...: a callback that wakes their connection). Every
// write to a key calls touched(key) with the key's stripe lock held, so a
// check made under the same lock followed by registering never misses a
// change. With nobody registered, touched() is one atomic load.
class KeyWatch {
 public:
  struct Watcher {
    std::atomic<bool> dirty{false};
  };

  // Fires once; the wake-up may be set before or after it fires.
  class Waiter {
   public:
    void set_wake(std::function<void()> wake) {
      std::unique_lock lock(mu_);
      if (fired_) {
        lock.unlock();
        wake();
        return;
      }
      wake_ = std::move(wake);
    }
    void fire() {
      std::function<void()> wake;
      {
        std::lock_guard lock(mu_);
        if (fired_) return;
        fired_ = true;
        wake = std::move(wake_);
      }
      if (wake) wake();
    }
    bool fired() const {
      std::lock_guard lock(mu_);
      return fired_;
    }

   private:
    mutable std::mutex mu_;
    bool fired_ = false;
    std::function<void()> wake_;
  };

  void watch(std::string_view key, std::shared_ptr<Watcher> w) {
    std::lock_guard lock(mu_);
    entry(key).watchers.push_back(std::move(w));
  }
  void unwatch(std::string_view key, const Watcher* w) {
    remove(key, [&](Entry& e) { std::erase_if(e.watchers, [&](const auto& p) { return p.get() == w; }); });
  }
  void wait(std::string_view key, std::shared_ptr<Waiter> w) {
    std::lock_guard lock(mu_);
    entry(key).waiters.push_back(std::move(w));
  }
  void unwait(std::string_view key, const Waiter* w) {
    remove(key, [&](Entry& e) { std::erase_if(e.waiters, [&](const auto& p) { return p.get() == w; }); });
  }

  void touched(std::string_view key) {
    if (entries_.load(std::memory_order_acquire) == 0) return;
    std::vector<std::shared_ptr<Waiter>> wake;
    {
      std::lock_guard lock(mu_);
      auto it = keys_.find(key);
      if (it == keys_.end()) return;
      for (const auto& w : it->second.watchers) w->dirty.store(true, std::memory_order_release);
      wake.swap(it->second.waiters);  // each wakes once; it registers again if it blocks again
      if (it->second.watchers.empty()) {
        keys_.erase(it);
        entries_.fetch_sub(1, std::memory_order_release);
      }
    }
    for (const auto& w : wake) w->fire();
  }

  // FLUSHALL, a full sync: every key changed.
  void touched_all() {
    if (entries_.load(std::memory_order_acquire) == 0) return;
    std::vector<std::shared_ptr<Waiter>> wake;
    {
      std::lock_guard lock(mu_);
      for (auto& [key, e] : keys_) {
        for (const auto& w : e.watchers) w->dirty.store(true, std::memory_order_release);
        for (auto& w : e.waiters) wake.push_back(std::move(w));
        e.waiters.clear();
      }
      std::erase_if(keys_, [](const auto& kv) { return kv.second.watchers.empty(); });
      entries_.store(keys_.size(), std::memory_order_release);
    }
    for (const auto& w : wake) w->fire();
  }

  size_t keys() const { return entries_.load(std::memory_order_acquire); }

  // The server goes away: blocked connections are gone, and their wake-ups
  // hold executors of its io_context.
  void drop_waiters() {
    std::lock_guard lock(mu_);
    for (auto& [key, e] : keys_) e.waiters.clear();
    std::erase_if(keys_, [](const auto& kv) { return kv.second.watchers.empty(); });
    entries_.store(keys_.size(), std::memory_order_release);
  }

 private:
  struct Entry {
    std::vector<std::shared_ptr<Watcher>> watchers;
    std::vector<std::shared_ptr<Waiter>> waiters;
  };

  Entry& entry(std::string_view key) {
    auto it = keys_.find(key);
    if (it == keys_.end()) {
      it = keys_.emplace(std::string(key), Entry{}).first;
      entries_.fetch_add(1, std::memory_order_release);
    }
    return it->second;
  }

  template <class F>
  void remove(std::string_view key, F&& f) {
    std::lock_guard lock(mu_);
    auto it = keys_.find(key);
    if (it == keys_.end()) return;
    f(it->second);
    if (it->second.watchers.empty() && it->second.waiters.empty()) {
      keys_.erase(it);
      entries_.fetch_sub(1, std::memory_order_release);
    }
  }

  struct KeyHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return static_cast<size_t>(hash_bytes(s)); }
  };

  std::mutex mu_;
  std::unordered_map<std::string, Entry, KeyHash, std::equal_to<>> keys_;
  std::atomic<size_t> entries_{0};
};

// The keys a connection watches (WATCH); unwatched when it goes away.
class WatchedKeys {
 public:
  WatchedKeys(std::weak_ptr<KeyWatch> registry) : registry_(std::move(registry)) {}
  ~WatchedKeys() { clear(); }
  WatchedKeys(const WatchedKeys&) = delete;
  WatchedKeys& operator=(const WatchedKeys&) = delete;

  const std::shared_ptr<KeyWatch::Watcher>& watcher() const { return watcher_; }
  bool has(std::string_view key) const {
    for (const auto& k : keys_) {
      if (k == key) return true;
    }
    return false;
  }
  void add(std::string key) { keys_.push_back(std::move(key)); }
  bool dirty() const { return watcher_->dirty.load(std::memory_order_acquire); }
  void clear() {
    if (auto r = registry_.lock()) {
      for (const auto& k : keys_) r->unwatch(k, watcher_.get());
    }
    keys_.clear();
    watcher_->dirty.store(false, std::memory_order_release);
  }

 private:
  std::weak_ptr<KeyWatch> registry_;
  std::shared_ptr<KeyWatch::Watcher> watcher_ = std::make_shared<KeyWatch::Watcher>();
  std::vector<std::string> keys_;
};

}  // namespace datadam
