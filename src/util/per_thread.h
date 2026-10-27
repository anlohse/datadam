#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace datadam {

// Adds n to a counter that only the calling thread writes: a plain load and
// store, not an atomic read-modify-write. Readers on other threads load it
// relaxed and may see it a little behind.
inline void bump(std::atomic<uint64_t>& counter, uint64_t n = 1) {
  counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// Counters kept per thread, for the hot path: each thread that counts gets a
// block of its own (no cache line shared with another thread, no lock), and
// readers add the blocks up. A block outlives its thread, so totals keep what
// finished threads counted. Block's counters must be std::atomic, written with
// bump().
template <class Block>
class PerThread {
 public:
  PerThread() = default;
  PerThread(const PerThread&) = delete;
  PerThread& operator=(const PerThread&) = delete;

  // This thread's block, made on its first call.
  Block& local() {
    // One slot per thread and Block type: after the first call, a compare.
    // Ids are never reused, so a slot never points into a destroyed set.
    thread_local Slot slot;
    if (slot.owner != id_) {
      slot.block = &find_or_add();
      slot.owner = id_;
    }
    return *slot.block;
  }

  // Calls f(const Block&) for every thread's block.
  template <class F>
  void for_each(F&& f) const {
    std::lock_guard lock(mu_);
    for (const auto& [thread, block] : blocks_) f(*block);
  }

 private:
  struct Slot {
    uint64_t owner = 0;
    Block* block = nullptr;
  };

  static uint64_t next_id() {
    static std::atomic<uint64_t> last{0};
    return last.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  Block& find_or_add() {
    std::lock_guard lock(mu_);
    auto me = std::this_thread::get_id();
    for (auto& [thread, block] : blocks_) {
      if (thread == me) return *block;
    }
    blocks_.emplace_back(me, std::make_unique<Block>());
    return *blocks_.back().second;
  }

  const uint64_t id_ = next_id();
  mutable std::mutex mu_;
  std::vector<std::pair<std::thread::id, std::unique_ptr<Block>>> blocks_;
};

}  // namespace datadam
