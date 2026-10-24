#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace datadam {

// Admission filter of the object cache (the "doorkeeper" of TinyLFU): when
// the cache is full, a key gets in only if it was already a candidate
// recently, i.e. on its second access. Keys read or written once (a bulk
// load, a scan, the long tail of a large data set) then no longer push hot
// keys out. Two generations of a bloom filter with two hash bits each; the
// older one is cleared when the newer has seen `window` keys, so "recently"
// means within the last one to two windows.
class Doorkeeper {
 public:
  explicit Doorkeeper(uint64_t window) : window_(window < 65536 ? 65536 : window) {
    // About 8 bits per key of a window keeps false positives near 2%.
    uint64_t bits = 1;
    while (bits < window_ * 8) bits <<= 1;
    words_ = bits / 64;
    mask_ = bits - 1;
    for (auto& g : gen_) g = std::make_unique<std::atomic<uint64_t>[]>(words_);
  }

  // True if `hash` was seen within the window; otherwise remembers it and returns false.
  bool seen_or_add(uint64_t hash) {
    uint64_t a = hash & mask_, b = (hash >> 32 | hash << 32) * 0x9E3779B97F4A7C15ULL & mask_;
    int cur = current_.load(std::memory_order_acquire);
    for (int g : {cur, cur ^ 1}) {
      if (test(g, a) && test(g, b)) return true;
    }
    set(cur, a);
    set(cur, b);
    if (added_.fetch_add(1, std::memory_order_relaxed) + 1 >= window_) rotate();
    return false;
  }

 private:
  bool test(int g, uint64_t bit) const {
    return gen_[g][bit / 64].load(std::memory_order_relaxed) & (1ULL << (bit % 64));
  }
  void set(int g, uint64_t bit) { gen_[g][bit / 64].fetch_or(1ULL << (bit % 64), std::memory_order_relaxed); }

  void rotate() {
    std::lock_guard lock(rotate_mu_);
    if (added_.load(std::memory_order_relaxed) < window_) return;  // another thread just did
    int older = current_.load(std::memory_order_relaxed) ^ 1;
    for (uint64_t i = 0; i < words_; ++i) gen_[older][i].store(0, std::memory_order_relaxed);
    current_.store(older, std::memory_order_release);
    added_.store(0, std::memory_order_relaxed);
  }

  uint64_t window_;
  uint64_t words_ = 0;
  uint64_t mask_ = 0;
  std::unique_ptr<std::atomic<uint64_t>[]> gen_[2];
  std::atomic<int> current_{0};
  std::atomic<uint64_t> added_{0};
  std::mutex rotate_mu_;
};

}  // namespace datadam
