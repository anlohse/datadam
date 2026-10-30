#pragma once

#include <mutex>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace datadam {

// Tells the CPU this is a spin-wait loop.
inline void cpu_relax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#else
  std::this_thread::yield();
#endif
}

// Tries to take m for a short while without sleeping. Key locks are held for
// microseconds, much less than a sleep and wake-up through the kernel costs.
inline bool spin_try_lock(std::mutex& m, int spins = 2000) {
  for (int i = 0; i < spins; ++i) {
    if (m.try_lock()) return true;
    cpu_relax();
  }
  return false;
}

// A lock_guard for a mutex held for well under a microsecond, as a cache
// shard's: spins a while before sleeping. A waiter that sleeps takes
// microseconds to wake, and while it does its CPU idles: in-memory GETs and
// SETs on 8 threads left a third of the CPU idle, waiting in futex_wait.
class SpinLockGuard {
 public:
  explicit SpinLockGuard(std::mutex& m, int spins = 200) : m_(m) {
    if (!spin_try_lock(m_, spins)) m_.lock();
  }
  ~SpinLockGuard() { m_.unlock(); }
  SpinLockGuard(const SpinLockGuard&) = delete;
  SpinLockGuard& operator=(const SpinLockGuard&) = delete;

 private:
  std::mutex& m_;
};

}  // namespace datadam
