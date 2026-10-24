#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace datadam {

// Geometric chunk sizes (like memcached slab classes). An allocation of n bytes
// is served by the smallest class >= n, so internal waste is bounded by the
// growth factor.
class SizeClasses {
 public:
  static constexpr size_t kMaxClasses = 64;
  static constexpr size_t kAlignment = 8;

  SizeClasses(size_t min_chunk, size_t max_chunk, double factor = 1.25);

  size_t count() const { return sizes_.size(); }
  size_t size(size_t cls) const { return sizes_[cls]; }
  size_t min_chunk() const { return sizes_.front(); }
  size_t max_chunk() const { return sizes_.back(); }
  // Class index for an allocation of `bytes`, or -1 when it exceeds max_chunk().
  int class_for(size_t bytes) const;

 private:
  std::vector<size_t> sizes_;
};

}  // namespace datadam
