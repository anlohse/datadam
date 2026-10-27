#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "util/hash.h"
#include "util/spin.h"

namespace datadam {

// Striped per-key locks. A writer (or a reader filling the cache after a miss)
// holds the key's stripe across the disk access and the cache update, so a
// slow reader can never put a stale value back into the cache.
//
// Over the stripes there is a world lock. A thread holds it shared for as
// long as it holds any stripe (taken when it goes from none to one). A Lua
// script, or a MULTI block that touches keys it does not name, takes it
// exclusively, holding no stripe: it then has the whole keyspace to itself,
// and takes no stripe at all until it releases it. A thread that waits for
// the world lock holds no stripe, so this adds no deadlock; writers go first
// (new shared holders wait for a waiting writer), so a script is not starved.
//
// A script commits without the world lock (downgrade_world): it keeps the
// stripes of the keys it used, marked `committing`, and does not count as a
// shared holder, so the next script can start while the first one's writes
// go to RocksDB. That next owner waits for a committing stripe before it uses
// a key there (wait_committed), and for every commit (wait_all_committed)
// before it uses keys it cannot name.
class KeyLocks {
 public:
  static constexpr size_t kStripes = 4096;

  size_t stripe_of(std::string_view key) const { return hash_bytes(key) & (kStripes - 1); }

  void lock(size_t s) {
    Holding& h = holding();
    if (h.world) return;
    if (h.stripes++ == 0) lock_world_shared();
    stripes_[s].mu.lock();
  }
  bool try_lock(size_t s) {
    Holding& h = holding();
    if (h.world) return true;
    bool first = h.stripes == 0;
    if (first) lock_world_shared();
    if (stripes_[s].mu.try_lock()) {
      ++h.stripes;
      return true;
    }
    if (first) unlock_world_shared();
    return false;
  }
  // Tries for a short while without sleeping: stripes are held for microseconds.
  bool spin_try_lock(size_t s, int spins = 2000) {
    Holding& h = holding();
    if (h.world) return true;
    bool first = h.stripes == 0;
    if (first) lock_world_shared();
    for (int i = 0; i < spins; ++i) {
      if (stripes_[s].mu.try_lock()) {
        ++h.stripes;
        return true;
      }
      cpu_relax();
    }
    if (first) unlock_world_shared();
    return false;
  }
  void unlock(size_t s) {
    Holding& h = holding();
    if (h.world) return;
    if (h.committing) stripes_[s].committing.store(false, std::memory_order_release);
    stripes_[s].mu.unlock();
    if (--h.stripes == 0) {
      if (h.committing) {
        h.committing = false;
        committers_.fetch_sub(1, std::memory_order_seq_cst);
        committers_.notify_all();
      } else {
        unlock_world_shared();
      }
    }
  }

  // The whole keyspace for this thread, which must hold no stripe.
  void lock_world() {
    Holding& h = holding();
    // One owner at a time, the others wait here (unlock_world releases it).
    // Scripts are short: spin a while before sleeping, so the next one starts
    // without a wake-up through the kernel.
    bool owner = false;
    for (int i = 0; i < kWorldSpins && !owner; ++i) {
      owner = writers_.try_lock();
      if (!owner) cpu_relax();
    }
    if (!owner) writers_.lock();
    writer_.store(true, std::memory_order_seq_cst);
    // Shared holders keep stripes for microseconds: spin before sleeping. A
    // reader that comes in after a slot was seen empty sees writer_ and steps
    // back (lock_world_shared), so one pass over the slots is enough.
    for (ReaderSlot& slot : readers_) {
      for (int i = 0; i < kWorldSpins && slot.n.load() != 0; ++i) cpu_relax();
      for (int64_t n = slot.n.load(); n != 0; n = slot.n.load()) slot.n.wait(n);
    }
    h.world = true;
  }
  // The world owner, before it uses a key of stripe s: waits until a script
  // still committing there (downgrade_world) has finished.
  void wait_committed(size_t s) {
    if (committers_.load(std::memory_order_acquire) == 0) return;
    if (!stripes_[s].committing.load(std::memory_order_acquire)) return;
    stripes_[s].mu.lock();  // the committer's: free once it is done
    stripes_[s].mu.unlock();
  }
  // The world owner, before it uses keys it does not name: waits for every
  // committing script.
  void wait_all_committed() {
    for (int i = 0; i < kWorldSpins && committers_.load() != 0; ++i) cpu_relax();
    for (int n = committers_.load(); n != 0; n = committers_.load()) committers_.wait(n);
  }
  void unlock_world() {
    Holding& h = holding();
    h.world = false;
    writer_.store(false, std::memory_order_seq_cst);
    writer_.notify_all();
    writers_.unlock();
  }
  bool world_owned() const { return const_cast<KeyLocks*>(this)->holding().world; }
  // The world owner keeps only these stripes (ascending, distinct), marked
  // committing, and gives the rest of the keyspace back; it releases them
  // with unlock(), and must take no other stripe meanwhile. Each is free: no
  // shared holder is left, and the owner waited (wait_committed) for any
  // earlier committer there.
  void downgrade_world(std::span<const size_t> stripes) {
    Holding& h = holding();
    for (size_t s : stripes) {
      stripes_[s].mu.lock();
      stripes_[s].committing.store(true, std::memory_order_release);
    }
    h.world = false;
    h.stripes = static_cast<int>(stripes.size());
    if (h.stripes > 0) {
      h.committing = true;
      committers_.fetch_add(1, std::memory_order_seq_cst);
    }
    writer_.store(false, std::memory_order_seq_cst);
    writer_.notify_all();
    writers_.unlock();
  }

  // Locks several stripes in ascending order (deadlock-free).
  class Multi {
   public:
    Multi(KeyLocks& locks, std::vector<size_t> stripes) : locks_(locks), stripes_(std::move(stripes)) {
      std::sort(stripes_.begin(), stripes_.end());
      stripes_.erase(std::unique(stripes_.begin(), stripes_.end()), stripes_.end());
      for (size_t s : stripes_) locks_.lock(s);
    }
    ~Multi() {
      for (auto it = stripes_.rbegin(); it != stripes_.rend(); ++it) locks_.unlock(*it);
      if (world_) locks_.unlock_world();
    }
    // Movable for `cond ? lock_all() : lock_keys(...)` (MSVC 2022 wants it there); a moved-from one unlocks nothing.
    Multi(Multi&& o) noexcept : locks_(o.locks_), stripes_(std::move(o.stripes_)), world_(o.world_) {
      o.stripes_.clear();
      o.world_ = false;
    }
    Multi(const Multi&) = delete;
    Multi& operator=(const Multi&) = delete;

   private:
    friend class KeyLocks;
    struct World {};
    explicit Multi(KeyLocks& locks, World) : locks_(locks), world_(true) {
      locks_.lock_world();
      locks_.wait_all_committed();
    }

    KeyLocks& locks_;
    std::vector<size_t> stripes_;
    bool world_ = false;
  };

  template <class Keys>
  Multi lock_keys(const Keys& keys) {
    std::vector<size_t> s;
    for (std::string_view k : keys) s.push_back(stripe_of(k));
    return Multi(*this, std::move(s));
  }

  // The whole keyspace (the world lock, and every committing script done),
  // for a thread that holds no stripe. Not the kStripes stripe mutexes one by
  // one: ThreadSanitizer's deadlock detector tracks at most 4096 held locks,
  // and spun for hours on FLUSHALL.
  Multi lock_all() { return Multi(*this, Multi::World{}); }

 private:
  static constexpr size_t kReaderSlots = 64;
  struct alignas(64) ReaderSlot {
    std::atomic<int64_t> n{0};
  };

  // What this thread holds of this KeyLocks.
  struct Holding {
    const KeyLocks* owner = nullptr;
    int stripes = 0;
    bool world = false;
    bool committing = false;  // its stripes are a downgraded world owner's
  };
  Holding& holding() {
    // A thread rarely works with more than one Database (tests run two servers in one process).
    thread_local std::array<Holding, 8> slots;
    Holding* free_slot = nullptr;
    for (auto& h : slots) {
      if (h.owner == this) return h;
      if (!free_slot && h.owner == nullptr) free_slot = &h;
    }
    if (!free_slot) {  // reuse a slot that holds nothing
      for (auto& h : slots) {
        if (h.stripes == 0 && !h.world) free_slot = &h;
      }
    }
    free_slot->owner = this;
    free_slot->stripes = 0;
    free_slot->world = false;
    free_slot->committing = false;
    return *free_slot;
  }

  // Shared holders count themselves in one of kReaderSlots counters, each on
  // its own cache line: a single counter, which every I/O thread changes for
  // every batch, would bounce between the cores of a big machine. A thread
  // keeps its slot; threads beyond kReaderSlots share them.
  ReaderSlot& reader_slot() {
    static std::atomic<size_t> next{0};
    thread_local size_t mine = next.fetch_add(1, std::memory_order_relaxed) % kReaderSlots;
    return readers_[mine];
  }
  void lock_world_shared() {
    ReaderSlot& slot = reader_slot();
    for (;;) {
      while (writer_.load(std::memory_order_seq_cst)) writer_.wait(true);
      slot.n.fetch_add(1, std::memory_order_seq_cst);
      if (!writer_.load(std::memory_order_seq_cst)) return;
      // A writer came in between: step back so it can go first.
      unlock_world_shared();
    }
  }
  void unlock_world_shared() {
    ReaderSlot& slot = reader_slot();
    if (slot.n.fetch_sub(1, std::memory_order_seq_cst) == 1 && writer_.load(std::memory_order_seq_cst)) {
      slot.n.notify_all();
    }
  }

  static constexpr int kWorldSpins = 20000;

  struct alignas(64) Stripe {
    std::mutex mu;
    std::atomic<bool> committing{false};  // held by a script committing (downgrade_world)
  };
  std::array<Stripe, kStripes> stripes_;
  std::array<ReaderSlot, kReaderSlots> readers_;
  alignas(64) std::atomic<bool> writer_{false};
  std::mutex writers_;
  alignas(64) std::atomic<int> committers_{0};  // downgraded owners still committing
};

}  // namespace datadam
