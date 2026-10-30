#include <gtest/gtest.h>

#include "server/resp.h"

using namespace datadam;

TEST(RespParser, Multibulk) {
  std::string buf = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n*1\r\n$4\r\nPING\r\n";
  size_t pos = 0;
  std::vector<std::string_view> args;
  std::string err;
  ASSERT_EQ(RespParser::parse(buf, pos, args, err), RespParser::Status::Ok);
  ASSERT_EQ(args.size(), 3u);
  EXPECT_EQ(args[0], "SET");
  EXPECT_EQ(args[2], "hello");
  ASSERT_EQ(RespParser::parse(buf, pos, args, err), RespParser::Status::Ok);
  EXPECT_EQ(args[0], "PING");
  EXPECT_EQ(pos, buf.size());
  EXPECT_EQ(RespParser::parse(buf, pos, args, err), RespParser::Status::Incomplete);
}

TEST(RespParser, IncompleteAtEveryByte) {
  std::string full = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
  for (size_t n = 0; n < full.size(); ++n) {
    size_t pos = 0;
    std::vector<std::string_view> args;
    std::string err;
    EXPECT_EQ(RespParser::parse(std::string_view(full).substr(0, n), pos, args, err),
              RespParser::Status::Incomplete)
        << n;
    EXPECT_EQ(pos, 0u);
  }
}

TEST(RespParser, IncompleteSaysHowMuchMoreItNeeds) {
  std::vector<std::string_view> args;
  std::string err;
  size_t pos = 0, need = 0;
  // Two arguments still to come: at least "$0\r\n\r\n" each.
  std::string buf = "*3\r\n$3\r\nSET\r\n";
  EXPECT_EQ(RespParser::parse(buf, pos, args, err, &need), RespParser::Status::Incomplete);
  EXPECT_EQ(need, buf.size() + 12);
  // Inside a bulk: its end, then the last argument.
  buf += "$100\r\nab";
  EXPECT_EQ(RespParser::parse(buf, pos, args, err, &need), RespParser::Status::Incomplete);
  EXPECT_EQ(need, buf.size() - 2 + 102 + 6);
  // A bulk header cut short: at least the smallest argument from there.
  buf = "*1\r\n$10";
  EXPECT_EQ(RespParser::parse(buf, pos, args, err, &need), RespParser::Status::Incomplete);
  EXPECT_EQ(need, 4u + 6);
  EXPECT_EQ(pos, 0u);
  // A header alone reserves no room for a million arguments.
  EXPECT_EQ(RespParser::parse("*1048576\r\n", pos, args, err), RespParser::Status::Incomplete);
  EXPECT_LE(args.capacity(), 1024u);
}

TEST(RespParser, BinarySafeValues) {
  std::string buf = std::string("*1\r\n$4\r\na\r\0b\r\n", 14);
  size_t pos = 0;
  std::vector<std::string_view> args;
  std::string err;
  ASSERT_EQ(RespParser::parse(buf, pos, args, err), RespParser::Status::Ok);
  EXPECT_EQ(args[0], std::string_view("a\r\0b", 4));
}

TEST(RespParser, Inline) {
  std::string buf = "set  foo bar\r\nPING\n";
  size_t pos = 0;
  std::vector<std::string_view> args;
  std::string err;
  ASSERT_EQ(RespParser::parse(buf, pos, args, err), RespParser::Status::Ok);
  ASSERT_EQ(args.size(), 3u);
  EXPECT_EQ(args[1], "foo");
  ASSERT_EQ(RespParser::parse(buf, pos, args, err), RespParser::Status::Ok);
  EXPECT_EQ(args[0], "PING");
}

TEST(RespParser, Errors) {
  size_t pos = 0;
  std::vector<std::string_view> args;
  std::string err;
  EXPECT_EQ(RespParser::parse("*1\r\n+x\r\n", pos, args, err), RespParser::Status::Error);
  pos = 0;
  EXPECT_EQ(RespParser::parse("*x\r\n", pos, args, err), RespParser::Status::Error);
  pos = 0;
  EXPECT_EQ(RespParser::parse("*1\r\n$3\r\nabcXY", pos, args, err), RespParser::Status::Error);
}

TEST(Reply, Resp2AndResp3) {
  std::string out;
  Reply r2(out, 2);
  r2.null();
  r2.map(1);
  r2.integer(-5);
  EXPECT_EQ(out, "$-1\r\n*2\r\n:-5\r\n");
  out.clear();
  Reply r3(out, 3);
  r3.null();
  r3.map(1);
  r3.double_value(1.5);
  EXPECT_EQ(out, "_\r\n%1\r\n,1.5\r\n");
}
