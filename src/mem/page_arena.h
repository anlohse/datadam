#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace datadam {

// Bookkeeping for one page. Only the slab allocator that currently owns the
// page reads or writes it (under that allocator's shard lock).
struct PageMeta {
  static constexpr uint32_t kNone = UINT32_MAX;
  static constexpr uint16_t kNoOwner = UINT16_MAX;

  uint32_t prev = kNone;       // links in the owner's "pages with free chunks" list
  uint32_t next = kNone;
  uint32_t free_head = kNone;  // first recycled chunk (index inside the page)
  uint32_t bump = 0;           // chunks [0, bump) have been handed out at least once
  uint32_t live = 0;           // chunks currently allocated
  uint32_t capacity = 0;       // chunks that fit in the page
  uint32_t chunk_size = 0;
  uint8_t cls = 0;
  uint16_t owner = kNoOwner;
};

// The memory pool's backing store: one contiguous address range reserved up
// front (cache-max-memory) and split into fixed-size pages. Pages are committed
// lazily and decommitted when freed, except that the first cache-min-memory
// worth of committed pages is kept for reuse.
class PageArena {
 public:
  static constexpr uint32_t kNone = PageMeta::kNone;

  PageArena(size_t max_bytes, size_t min_bytes, size_t page_size, size_t min_chunk);
  ~PageArena();
  PageArena(const PageArena&) = delete;
  PageArena& operator=(const PageArena&) = delete;

  // Returns a page index, or kNone when the budget is exhausted.
  uint32_t acquire();
  void release(uint32_t page);

  char* page_ptr(uint32_t page) const { return base_ + static_cast<size_t>(page) * page_size_; }
  uint32_t page_of(const void* p) const {
    return static_cast<uint32_t>((static_cast<const char*>(p) - base_) / page_size_);
  }
  PageMeta& meta(uint32_t page) { return metas_[page]; }
  // One bit per chunk slot, set while the chunk is allocated.
  uint64_t* live_bits(uint32_t page) { return bits_.data() + static_cast<size_t>(page) * words_per_page_; }
  size_t words_per_page() const { return words_per_page_; }

  size_t page_size() const { return page_size_; }
  size_t max_pages() const { return max_pages_; }
  size_t min_pages() const { return min_pages_; }
  size_t pages_in_use() const { return in_use_.load(std::memory_order_relaxed); }
  size_t pages_committed() const { return committed_.load(std::memory_order_relaxed); }

 private:
  bool commit(uint32_t page);
  void decommit(uint32_t page);

  char* base_ = nullptr;
  size_t page_size_;
  size_t max_pages_;
  size_t min_pages_;
  size_t words_per_page_;
  std::vector<PageMeta> metas_;
  std::vector<uint64_t> bits_;

  std::mutex mu_;
  std::vector<uint32_t> free_committed_;    // free pages still backed by memory
  std::vector<uint32_t> free_decommitted_;  // free pages returned to the OS
  uint32_t next_fresh_ = 0;                 // pages >= this were never used
  std::atomic<size_t> in_use_{0};
  std::atomic<size_t> committed_{0};
};

}  // namespace datadam
