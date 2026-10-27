#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace datadam {

// Incremental RESP request parser. Handles multibulk requests (what client
// libraries send) and inline commands (what you type in telnet).
class RespParser {
 public:
  enum class Status { Ok, Incomplete, Error };

  static constexpr size_t kMaxInlineSize = 64 * 1024;
  static constexpr int64_t kMaxBulkSize = 512LL << 20;
  static constexpr int64_t kMaxArgs = 1 << 20;

  // Parses one request starting at buf[pos]. On Ok, `args` holds views into
  // `buf` and pos is advanced past the request. An Ok with no args is an empty
  // line or "*0", which the caller should skip.
  static Status parse(std::string_view buf, size_t& pos, std::vector<std::string_view>& args, std::string& error);
};

// Appends RESP2/RESP3 replies to an output buffer.
class Reply {
 public:
  Reply(std::string& out, int proto) : out_(out), proto_(proto) {}

  int proto() const { return proto_; }
  void set_proto(int p) { proto_ = p; }

  void ok() { simple("OK"); }
  void simple(std::string_view s);
  // `msg` starts with the error code, e.g. "ERR syntax error".
  void error(std::string_view msg);
  void integer(int64_t v);
  void bulk(std::string_view s);
  void null();          // missing value: $-1 (RESP2) or _ (RESP3)
  void null_array();    // *-1 (RESP2) or _ (RESP3)
  void array(size_t n);
  void map(size_t n);   // RESP3 map, flattened to an array of 2n in RESP2
  void set(size_t n);   // RESP3 set, an array in RESP2
  void push(size_t n);  // RESP3 push (Pub/Sub messages), an array in RESP2
  void double_value(double v);
  void raw(std::string_view s) { out_.append(s); }
  // Undoes what was appended since mark() (a command that will be run again).
  size_t mark() const { return out_.size(); }
  void rewind(size_t mark) { out_.resize(mark); }
  // What has been written (Pub/Sub hands it to the connection's subscriber).
  std::string& buffer() { return out_; }
  // Errors replied so far, and the code (first word) of the last one, for
  // INFO errorstats.
  uint64_t errors() const { return errors_; }
  std::string_view last_error_code() const { return last_error_code_; }

 private:
  void header(char type, int64_t n);

  std::string& out_;
  int proto_;
  uint64_t errors_ = 0;
  std::string last_error_code_;
};

}  // namespace datadam
