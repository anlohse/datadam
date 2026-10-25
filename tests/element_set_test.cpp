#include <gtest/gtest.h>

#include <map>
#include <random>

#include "db/element_set.h"

using namespace datadam;

namespace {

std::vector<std::pair<std::string, std::string>> dump(const ElementSet& s, std::string_view from = "",
                                                     std::string_view to = "", bool reverse = false) {
  std::vector<std::pair<std::string, std::string>> out;
  s.for_range(from, to, reverse, [&](std::string_view k, std::string_view v) {
    out.emplace_back(k, v);
    return true;
  });
  return out;
}

}  // namespace

TEST(ElementSetTest, MatchesAMapThroughLoadsAndChanges) {
  std::mt19937 rng(11);
  std::map<std::string, std::string> model;
  ElementSet set;
  for (int round = 0; round < 200; ++round) {
    for (int i = 0; i < 20; ++i) {
      std::string k = "k" + std::to_string(rng() % 30);
      if (rng() % 3 == 0) {
        model.erase(k);
        set.erase(k);
      } else {
        std::string v(rng() % 20, static_cast<char>('a' + rng() % 26));
        model[k] = v;
        set.put(k, v);
      }
    }
    if (round % 3 == 0) {  // as when the next command loads it from the cache
      ElementSet reloaded;
      ASSERT_TRUE(reloaded.load(set.encode()));
      set = std::move(reloaded);
    }
    ASSERT_EQ(set.size(), model.size());
    std::vector<std::pair<std::string, std::string>> expected(model.begin(), model.end());
    ASSERT_EQ(dump(set), expected);
    std::vector<std::pair<std::string, std::string>> reversed(expected.rbegin(), expected.rend());
    ASSERT_EQ(dump(set, "", "", true), reversed);
    // A sub-range, both ways.
    std::vector<std::pair<std::string, std::string>> mid;
    for (const auto& [k, v] : model) {
      if (k >= "k1" && k < "k2") mid.emplace_back(k, v);
    }
    ASSERT_EQ(dump(set, "k1", "k2"), mid);
    std::reverse(mid.begin(), mid.end());
    ASSERT_EQ(dump(set, "k1", "k2", true), mid);
    for (const auto& [k, v] : model) ASSERT_EQ(set.get(k), std::optional<std::string_view>(v));
    ASSERT_FALSE(set.get("absent"));
  }
}

TEST(ElementSetTest, AppendBuildsAValidEncoding) {
  ElementSet set;
  set.append("a", "1");
  set.append("b", std::string(300, 'x'));
  ElementSet copy;
  ASSERT_TRUE(copy.load(set.encode()));
  EXPECT_EQ(copy.size(), 2u);
  EXPECT_EQ(copy.get("b")->size(), 300u);
  EXPECT_FALSE(copy.load(std::string("\x05\x00\x00\x00garbage", 11)));  // 5 elements announced, bytes missing
  EXPECT_EQ(copy.size(), 0u);
  EXPECT_FALSE(copy.load("\x05"));
  EXPECT_TRUE(copy.load(""));
}

// Found by fuzz_records: a count far beyond what the buffer can hold must be
// refused, not used to size anything (the encoding comes from disk and from a
// primary).
TEST(ElementSetTest, RefusesACountTheBufferCannotHold) {
  ElementSet s;
  EXPECT_FALSE(s.load(std::string("\x7f\x0a\x5d\x5d", 4)));
  EXPECT_EQ(s.size(), 0u);
  EXPECT_FALSE(s.load(std::string("\xff\xff\xff\xff", 4)));
  EXPECT_FALSE(s.load(std::string("\x02\x00\x00\x00\x00\x00\x00\x00", 8)));  // 2 elements, room for 0
  // Also found by fuzz_records: a value length of 0xffffffff, which a 32-bit
  // sum turned into 3, so the set loaded and was read far past its buffer.
  EXPECT_FALSE(s.load(std::string("\x01\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff", 12)));
  // And subs out of order (or repeated), which binary searches would miss.
  ElementSet good;
  good.append("a", "1");
  good.append("b", "2");
  std::string enc = good.encode();
  EXPECT_TRUE(s.load(enc));
  std::string swapped = enc;
  std::swap(swapped[swapped.find('a')], swapped[swapped.find('b')]);  // now "b" before "a"
  EXPECT_FALSE(s.load(swapped));
}
