#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>

namespace datadam {

// Lock-free histogram of durations in microseconds, for INFO latency. Each
// power of two is split into four buckets, so a percentile is reported with at
// most 25% error (the bucket's upper bound), from 1us to about a minute.
class LatencyHistogram {
 public:
  void record(uint64_t us) {
    buckets_[bucket_of(us)].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    uint64_t m = max_.load(std::memory_order_relaxed);
    while (us > m && !max_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
  }

  uint64_t count() const { return count_.load(std::memory_order_relaxed); }
  uint64_t max() const { return max_.load(std::memory_order_relaxed); }

  // The smallest bucket bound below which at least `p` (0..1) of the samples fall.
  uint64_t percentile(double p) const {
    std::array<uint64_t, kBuckets> snap;
    uint64_t total = 0;
    for (size_t i = 0; i < kBuckets; ++i) total += snap[i] = buckets_[i].load(std::memory_order_relaxed);
    if (total == 0) return 0;
    auto want = static_cast<uint64_t>(p * static_cast<double>(total));
    if (want < 1) want = 1;
    uint64_t seen = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      seen += snap[i];
      if (seen >= want) return std::min(upper_bound(i), max());
    }
    return max();
  }

  void reset() {
    for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_relaxed);
    max_.store(0, std::memory_order_relaxed);
  }

  // Measures the lifetime of the timer into `h`.
  class Timer {
   public:
    explicit Timer(LatencyHistogram& h) : h_(h), start_(std::chrono::steady_clock::now()) {}
    ~Timer() {
      auto d = std::chrono::steady_clock::now() - start_;
      h_.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count()));
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

   private:
    LatencyHistogram& h_;
    std::chrono::steady_clock::time_point start_;
  };

 private:
  static constexpr size_t kBuckets = 4 * 27;

  // Values below 4 get a bucket each; above, the bucket is the power of two
  // and the two bits below the leading one.
  static size_t bucket_of(uint64_t us) {
    if (us < 4) return us;
    int log = std::bit_width(us) - 1;
    size_t i = 4 * static_cast<size_t>(log - 1) + ((us >> (log - 2)) & 3);
    return std::min(i, kBuckets - 1);
  }
  static uint64_t upper_bound(size_t i) {
    if (i < 4) return i;
    int log = static_cast<int>(i / 4) + 1;
    return ((4 + i % 4 + 1) << (log - 2)) - 1;
  }

  std::array<std::atomic<uint64_t>, kBuckets> buckets_{};
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> max_{0};
};

}  // namespace datadam
