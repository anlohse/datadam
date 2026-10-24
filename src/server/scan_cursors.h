#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

namespace datadam {

// Where SCAN-family iterations stopped. Clients expect numeric cursors, so
// each cursor number stands for the last key (or element) returned plus what
// was being scanned. Any connection may continue a cursor, and using one twice
// continues from the same place. The table is bounded: the least recently used
// cursors are forgotten (continuing one of those is an error).
class ScanCursors {
 public:
  static constexpr size_t kMaxCursors = 100'000;

  // scope: what is scanned ("" for the keyspace, else the command and key).
  uint64_t save(std::string scope, std::string position) {
    std::lock_guard lock(mu_);
    uint64_t id;
    do {
      id = rng_() >> 1;  // 63 bits: clients may parse cursors as signed
    } while (id == 0 || by_id_.contains(id));
    lru_.push_front(id);
    by_id_.emplace(id, Entry{std::move(scope), std::move(position), lru_.begin()});
    if (by_id_.size() > kMaxCursors) {
      by_id_.erase(lru_.back());
      lru_.pop_back();
    }
    return id;
  }

  // The position saved for a cursor of this scope, or nullopt if unknown.
  std::optional<std::string> load(uint64_t id, std::string_view scope) {
    std::lock_guard lock(mu_);
    auto it = by_id_.find(id);
    if (it == by_id_.end() || it->second.scope != scope) return std::nullopt;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.position;
  }

 private:
  struct Entry {
    std::string scope;
    std::string position;
    std::list<uint64_t>::iterator lru;
  };

  std::mutex mu_;
  std::mt19937_64 rng_{std::random_device{}()};
  std::list<uint64_t> lru_;  // most recently used first
  std::unordered_map<uint64_t, Entry> by_id_;
};

}  // namespace datadam
