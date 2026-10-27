#include <gtest/gtest.h>

#include <cmath>

#include "util/latency_histogram.h"
#include "util/strings.h"

using namespace datadam;

TEST(Strings, ParseSize) {
  EXPECT_EQ(parse_size("4096"), 4096u);
  EXPECT_EQ(parse_size("4kb"), 4096u);
  EXPECT_EQ(parse_size("4K"), 4096u);
  EXPECT_EQ(parse_size("512mb"), 512ull << 20);
  EXPECT_EQ(parse_size("1g"), 1ull << 30);
  EXPECT_FALSE(parse_size("abc"));
  EXPECT_FALSE(parse_size("10xb"));
}

TEST(Strings, ParseDuration) {
  EXPECT_EQ(parse_duration("500ms")->count(), 500);
  EXPECT_EQ(parse_duration("30")->count(), 30'000);
  EXPECT_EQ(parse_duration("5m")->count(), 300'000);
  EXPECT_EQ(parse_duration("1h")->count(), 3'600'000);
  EXPECT_EQ(parse_duration("0")->count(), 0);
  EXPECT_FALSE(parse_duration("5 m"));
}

TEST(Strings, ParseNumbers) {
  EXPECT_EQ(parse_int64("-42"), -42);
  EXPECT_FALSE(parse_int64(" 1"));
  EXPECT_FALSE(parse_int64("1a"));
  EXPECT_FALSE(parse_int64("99999999999999999999"));
  EXPECT_FALSE(parse_uint64("-1"));
  EXPECT_DOUBLE_EQ(*parse_double("3.5"), 3.5);
  EXPECT_DOUBLE_EQ(*parse_double("+1.5"), 1.5);
  EXPECT_TRUE(std::isinf(*parse_double("-inf")));
  EXPECT_FALSE(parse_double("nan"));
  // Only the view's characters, not what follows them in memory.
  std::string buf = "1.5e5";
  EXPECT_EQ(parse_long_double(std::string_view(buf).substr(0, 3)), 1.5L);
  EXPECT_DOUBLE_EQ(*parse_double(std::string_view(buf).substr(0, 3)), 1.5);
  EXPECT_EQ(format_double(10.5), "10.5");
  EXPECT_EQ(format_long_double(3.0L), "3");
}

TEST(Strings, GlobMatch) {
  EXPECT_TRUE(glob_match("*", "anything"));
  EXPECT_TRUE(glob_match("user:*", "user:42"));
  EXPECT_FALSE(glob_match("user:*", "order:42"));
  EXPECT_TRUE(glob_match("h?llo", "hello"));
  EXPECT_TRUE(glob_match("h[ae]llo", "hallo"));
  EXPECT_FALSE(glob_match("h[^e]llo", "hello"));
  EXPECT_TRUE(glob_match("h[a-c]llo", "hbllo"));
  EXPECT_TRUE(glob_match("a*b*c", "aXXbYYc"));
  EXPECT_FALSE(glob_match("a*b*c", "aXXbYY"));
  EXPECT_TRUE(glob_match("a\\*", "a*"));
  EXPECT_TRUE(glob_match("CACHE-*", "cache-max-memory", true));
}

TEST(LatencyHistogram, PercentilesWithinABucket) {
  LatencyHistogram h;
  EXPECT_EQ(h.percentile(0.99), 0u);
  for (uint64_t us = 1; us <= 1000; ++us) h.record(us);
  EXPECT_EQ(h.count(), 1000u);
  EXPECT_EQ(h.max(), 1000u);
  // Upper bounds of the buckets: never below the true value, at most 25% above.
  for (auto [p, exact] : {std::pair{0.5, 500.0}, {0.99, 990.0}, {0.999, 999.0}}) {
    auto v = static_cast<double>(h.percentile(p));
    EXPECT_GE(v, exact) << p;
    EXPECT_LE(v, exact * 1.25) << p;
  }
  h.record(5'000'000);  // one slow outlier
  EXPECT_EQ(h.percentile(1.0), 5'000'000u);
  EXPECT_LE(h.percentile(0.99), 1250u);  // the outlier does not move p99
  h.reset();
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.max(), 0u);
}
