#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "mem/asan.h"
#include "mem/page_arena.h"
#include "mem/size_classes.h"
#include "mem/slab_allocator.h"

using namespace datadam;

namespace {
constexpr size_t kPage = 64 * 1024;
}

TEST(SizeClasses, GeometricAndBounded) {
  SizeClasses c(88, 4168);
  EXPECT_EQ(c.min_chunk(), 88u);
  EXPECT_EQ(c.max_chunk(), 4168u);
  EXPECT_LE(c.count(), SizeClasses::kMaxClasses);
  for (size_t i = 1; i < c.count(); ++i) EXPECT_GT(c.size(i), c.size(i - 1));
  EXPECT_EQ(c.class_for(1), 0);
  EXPECT_EQ(c.size(static_cast<size_t>(c.class_for(100))), c.size(1));
  EXPECT_EQ(c.class_for(4169), -1);
}

TEST(SlabAllocator, AllocatesDistinctChunksAndReleasesEmptyPages) {
  SizeClasses classes(64, 1024);
  PageArena arena(4 * kPage, 0, kPage, classes.min_chunk());
  SlabAllocator slab(arena, classes, 0);

  std::vector<void*> chunks;
  std::set<void*> unique;
  for (int i = 0; i < 100; ++i) {
    void* p = slab.allocate(0);
    ASSERT_NE(p, nullptr);
    chunks.push_back(p);
    unique.insert(p);
  }
  EXPECT_EQ(unique.size(), 100u);
  EXPECT_EQ(slab.pages_owned(), 1u);
  EXPECT_EQ(arena.pages_in_use(), 1u);
  EXPECT_EQ(slab.live_chunks(slab.page_of(chunks[0])).size(), 100u);

  for (void* p : chunks) slab.deallocate(p);
  EXPECT_EQ(slab.pages_owned(), 0u);
  EXPECT_EQ(arena.pages_in_use(), 0u);
  EXPECT_EQ(slab.bytes_allocated(), 0u);
}

TEST(SlabAllocator, ReusesFreedChunks) {
  SizeClasses classes(64, 1024);
  PageArena arena(kPage, 0, kPage, classes.min_chunk());
  SlabAllocator slab(arena, classes, 0);
  void* a = slab.allocate(0);
  void* b = slab.allocate(0);
  slab.deallocate(a);
  EXPECT_EQ(slab.allocate(0), a);
  slab.deallocate(b);
}

TEST(SlabAllocator, StopsAtBudget) {
  SizeClasses classes(64, 1024);
  PageArena arena(2 * kPage, 0, kPage, classes.min_chunk());
  SlabAllocator slab(arena, classes, 0);
  size_t per_page = kPage / classes.size(classes.count() - 1);
  size_t n = 0;
  while (slab.allocate(classes.count() - 1) != nullptr) ++n;
  EXPECT_EQ(n, 2 * per_page);
  EXPECT_EQ(arena.pages_in_use(), 2u);
}

TEST(PageArena, KeepsMinimumCommitted) {
  PageArena arena(4 * kPage, 2 * kPage, kPage, 64);
  uint32_t p[4];
  for (auto& x : p) x = arena.acquire();
  EXPECT_EQ(arena.acquire(), PageArena::kNone);
  EXPECT_EQ(arena.pages_committed(), 4u);
  for (auto x : p) arena.release(x);
  EXPECT_EQ(arena.pages_in_use(), 0u);
  EXPECT_EQ(arena.pages_committed(), 2u);  // min-memory stays committed
}

#ifdef DATADAM_ASAN
// In ASan builds the pool poisons what it does not hand out: touching a freed
// chunk (or the unused rest of a page) is reported, as a use-after-free would be.
TEST(SlabAllocatorDeathTest, FreedChunksArePoisoned) {
  SizeClasses classes(64, 1024);
  PageArena arena(kPage, 0, kPage, classes.min_chunk());
  SlabAllocator slab(arena, classes, 0);
  char* a = static_cast<char*>(slab.allocate(0));
  char* b = static_cast<char*>(slab.allocate(0));
  ASSERT_NE(a, nullptr);
  a[0] = 'x';
  slab.deallocate(a);
  EXPECT_DEATH(static_cast<void>(*static_cast<volatile char*>(a)), "use-after-poison");
  EXPECT_DEATH(static_cast<void>(*static_cast<volatile char*>(b + classes.size(0))), "use-after-poison");
  slab.deallocate(b);
}
#endif
