#include "mem/slab_allocator.h"

#include <bit>
#include <cstring>

#include "mem/asan.h"

namespace datadam {

SlabAllocator::SlabAllocator(PageArena& arena, const SizeClasses& classes, uint16_t owner_id)
    : arena_(arena), classes_(classes), owner_(owner_id) {
  for (auto& head : partial_) head = PageMeta::kNone;
}

SlabAllocator::~SlabAllocator() {
  // Only runs when the whole cache is torn down, so no other shard is
  // touching page metadata concurrently.
  for (uint32_t p = 0; p < arena_.max_pages() && pages_owned_ > 0; ++p) {
    PageMeta& m = arena_.meta(p);
    if (m.owner == owner_) {
      m.owner = PageMeta::kNoOwner;
      --pages_owned_;
      --class_pages_[m.cls];
      arena_.release(p);
    }
  }
}

void SlabAllocator::link_partial(uint32_t page) {
  PageMeta& m = arena_.meta(page);
  m.prev = PageMeta::kNone;
  m.next = partial_[m.cls];
  if (m.next != PageMeta::kNone) arena_.meta(m.next).prev = page;
  partial_[m.cls] = page;
}

void SlabAllocator::unlink_partial(uint32_t page) {
  PageMeta& m = arena_.meta(page);
  if (m.prev != PageMeta::kNone) arena_.meta(m.prev).next = m.next;
  else partial_[m.cls] = m.next;
  if (m.next != PageMeta::kNone) arena_.meta(m.next).prev = m.prev;
  m.prev = m.next = PageMeta::kNone;
}

void* SlabAllocator::allocate(size_t cls) {
  uint32_t page = partial_[cls];
  if (page == PageMeta::kNone) {
    page = arena_.acquire();
    if (page == PageArena::kNone) return nullptr;
    PageMeta& m = arena_.meta(page);
    m = PageMeta{};
    m.cls = static_cast<uint8_t>(cls);
    m.owner = owner_;
    m.chunk_size = static_cast<uint32_t>(classes_.size(cls));
    m.capacity = static_cast<uint32_t>(arena_.page_size() / m.chunk_size);
    std::memset(arena_.live_bits(page), 0, arena_.words_per_page() * sizeof(uint64_t));
    asan_poison(arena_.page_ptr(page), arena_.page_size());  // chunks are unpoisoned as they are handed out
    link_partial(page);
    ++pages_owned_;
    ++class_pages_[cls];
  }

  PageMeta& m = arena_.meta(page);
  char* base = arena_.page_ptr(page);
  uint32_t idx;
  if (m.free_head != PageMeta::kNone) {
    idx = m.free_head;
    asan_unpoison(base + static_cast<size_t>(idx) * m.chunk_size, m.chunk_size);
    std::memcpy(&m.free_head, base + static_cast<size_t>(idx) * m.chunk_size, sizeof(uint32_t));
  } else {
    idx = m.bump++;
    asan_unpoison(base + static_cast<size_t>(idx) * m.chunk_size, m.chunk_size);
  }
  arena_.live_bits(page)[idx / 64] |= 1ULL << (idx % 64);
  ++m.live;
  if (m.live == m.capacity) unlink_partial(page);

  bytes_allocated_ += m.chunk_size;
  ++chunks_allocated_;
  return base + static_cast<size_t>(idx) * m.chunk_size;
}

void SlabAllocator::deallocate(void* chunk) {
  uint32_t page = arena_.page_of(chunk);
  PageMeta& m = arena_.meta(page);
  char* base = arena_.page_ptr(page);
  uint32_t idx = static_cast<uint32_t>((static_cast<char*>(chunk) - base) / m.chunk_size);

  arena_.live_bits(page)[idx / 64] &= ~(1ULL << (idx % 64));
  bytes_allocated_ -= m.chunk_size;
  --chunks_allocated_;
  bool was_full = m.live == m.capacity;
  --m.live;

  if (m.live == 0) {
    // Empty pages go back to the arena so any size class (or shard) can use them.
    if (!was_full) unlink_partial(page);
    m.owner = PageMeta::kNoOwner;
    --pages_owned_;
    --class_pages_[m.cls];
    asan_poison(base, arena_.page_size());
    arena_.release(page);
    return;
  }
  std::memcpy(chunk, &m.free_head, sizeof(uint32_t));
  asan_poison(chunk, m.chunk_size);  // the free-list link inside it is read only after unpoisoning
  m.free_head = idx;
  if (was_full) link_partial(page);
}

std::vector<void*> SlabAllocator::live_chunks(uint32_t page) {
  std::vector<void*> out;
  PageMeta& m = arena_.meta(page);
  char* base = arena_.page_ptr(page);
  const uint64_t* bits = arena_.live_bits(page);
  out.reserve(m.live);
  for (uint32_t w = 0; w * 64 < m.bump; ++w) {
    uint64_t word = bits[w];
    while (word != 0) {
      unsigned bit = static_cast<unsigned>(std::countr_zero(word));
      out.push_back(base + static_cast<size_t>(w * 64 + bit) * m.chunk_size);
      word &= word - 1;
    }
  }
  return out;
}

}  // namespace datadam
