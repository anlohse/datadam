#include "mem/size_classes.h"

#include <algorithm>
#include <stdexcept>

namespace datadam {

namespace {
size_t align_up(size_t n) { return (n + SizeClasses::kAlignment - 1) & ~(SizeClasses::kAlignment - 1); }
}  // namespace

SizeClasses::SizeClasses(size_t min_chunk, size_t max_chunk, double factor) {
  min_chunk = align_up(std::max<size_t>(min_chunk, kAlignment));
  max_chunk = align_up(std::max(max_chunk, min_chunk));
  // Widen the factor until the classes fit in kMaxClasses.
  for (;;) {
    sizes_.clear();
    size_t s = min_chunk;
    while (s < max_chunk) {
      sizes_.push_back(s);
      s = align_up(std::max(static_cast<size_t>(static_cast<double>(s) * factor), s + kAlignment));
    }
    sizes_.push_back(max_chunk);
    if (sizes_.size() <= kMaxClasses) break;
    factor *= 1.1;
  }
}

int SizeClasses::class_for(size_t bytes) const {
  auto it = std::lower_bound(sizes_.begin(), sizes_.end(), bytes);
  if (it == sizes_.end()) return -1;
  return static_cast<int>(it - sizes_.begin());
}

}  // namespace datadam
