#pragma once

// AddressSanitizer sees the memory pool as one big live allocation, so it
// cannot tell a freed chunk from a live one. The pool marks (poisons) what it
// does not hand out: a read or write there is then reported like a
// use-after-free. Without ASan these do nothing.

#include <cstddef>

#if defined(__SANITIZE_ADDRESS__)
#define DATADAM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DATADAM_ASAN 1
#endif
#endif

#ifdef DATADAM_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace datadam {

inline void asan_poison(const void* p, size_t n) {
#ifdef DATADAM_ASAN
  ASAN_POISON_MEMORY_REGION(p, n);
#else
  (void)p;
  (void)n;
#endif
}

inline void asan_unpoison(const void* p, size_t n) {
#ifdef DATADAM_ASAN
  ASAN_UNPOISON_MEMORY_REGION(p, n);
#else
  (void)p;
  (void)n;
#endif
}

}  // namespace datadam
