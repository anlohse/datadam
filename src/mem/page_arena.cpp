#include "mem/page_arena.h"

#include <algorithm>
#include <new>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace datadam {

PageArena::PageArena(size_t max_bytes, size_t min_bytes, size_t page_size, size_t min_chunk)
    : page_size_(page_size),
      max_pages_(max_bytes / page_size),
      min_pages_(std::min(min_bytes / page_size, max_bytes / page_size)),
      words_per_page_((page_size / min_chunk + 63) / 64) {
  metas_.resize(max_pages_);
  bits_.resize(max_pages_ * words_per_page_);
  free_committed_.reserve(max_pages_);
  if (max_pages_ == 0) return;

  size_t bytes = max_pages_ * page_size_;
#ifdef _WIN32
  base_ = static_cast<char*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS));
  if (base_ == nullptr) throw std::bad_alloc();
#else
  // MAP_NORESERVE: the kernel backs pages only when touched.
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  base_ = static_cast<char*>(p);
#endif
}

PageArena::~PageArena() {
  if (base_ == nullptr) return;
#ifdef _WIN32
  VirtualFree(base_, 0, MEM_RELEASE);
#else
  munmap(base_, max_pages_ * page_size_);
#endif
}

bool PageArena::commit(uint32_t page) {
#ifdef _WIN32
  return VirtualAlloc(page_ptr(page), page_size_, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
  (void)page;
  return true;
#endif
}

void PageArena::decommit(uint32_t page) {
#ifdef _WIN32
  VirtualFree(page_ptr(page), page_size_, MEM_DECOMMIT);
#else
  madvise(page_ptr(page), page_size_, MADV_DONTNEED);
#endif
}

uint32_t PageArena::acquire() {
  std::lock_guard lock(mu_);
  uint32_t page;
  if (!free_committed_.empty()) {
    page = free_committed_.back();
    free_committed_.pop_back();
  } else {
    if (!free_decommitted_.empty()) {
      page = free_decommitted_.back();
      free_decommitted_.pop_back();
    } else if (next_fresh_ < max_pages_) {
      page = next_fresh_++;
    } else {
      return kNone;
    }
    if (!commit(page)) {
      free_decommitted_.push_back(page);
      return kNone;
    }
    committed_.fetch_add(1, std::memory_order_relaxed);
  }
  in_use_.fetch_add(1, std::memory_order_relaxed);
  return page;
}

void PageArena::release(uint32_t page) {
  std::lock_guard lock(mu_);
  in_use_.fetch_sub(1, std::memory_order_relaxed);
  // Keep up to min_pages committed so a busy cache does not pay page faults
  // again; give the rest back to the OS.
  if (committed_.load(std::memory_order_relaxed) > min_pages_) {
    decommit(page);
    committed_.fetch_sub(1, std::memory_order_relaxed);
    free_decommitted_.push_back(page);
  } else {
    free_committed_.push_back(page);
  }
}

}  // namespace datadam
