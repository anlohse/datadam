#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mem/page_arena.h"
#include "mem/size_classes.h"

namespace datadam {

// Per-shard slab allocator. Each page it takes from the arena is dedicated to
// one size class and carved into equal chunks. Not thread-safe: the owning
// cache shard serializes access with its own lock.
class SlabAllocator {
 public:
  SlabAllocator(PageArena& arena, const SizeClasses& classes, uint16_t owner_id);
  ~SlabAllocator();
  SlabAllocator(const SlabAllocator&) = delete;
  SlabAllocator& operator=(const SlabAllocator&) = delete;

  // Returns nullptr when there is no free chunk and the arena has no page left.
  void* allocate(size_t cls);
  void deallocate(void* chunk);

  uint32_t page_of(const void* chunk) const { return arena_.page_of(chunk); }
  // Live chunk addresses of a page owned by this allocator.
  std::vector<void*> live_chunks(uint32_t page);

  size_t pages_owned() const { return pages_owned_; }
  size_t pages_in_class(size_t cls) const { return class_pages_[cls]; }
  size_t bytes_allocated() const { return bytes_allocated_; }
  size_t chunks_allocated() const { return chunks_allocated_; }

 private:
  void link_partial(uint32_t page);
  void unlink_partial(uint32_t page);

  PageArena& arena_;
  const SizeClasses& classes_;
  uint16_t owner_;
  uint32_t partial_[SizeClasses::kMaxClasses];  // heads of "pages with free chunks" lists
  size_t class_pages_[SizeClasses::kMaxClasses] = {};
  size_t pages_owned_ = 0;
  size_t bytes_allocated_ = 0;
  size_t chunks_allocated_ = 0;
};

}  // namespace datadam
