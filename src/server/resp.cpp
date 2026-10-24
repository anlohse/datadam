#include "server/resp.h"

#include <charconv>
#include <format>

#include "util/strings.h"

namespace datadam {

namespace {

// Parses the decimal number in buf[start, crlf); RESP lengths may be negative.
bool parse_len(std::string_view buf, size_t start, size_t crlf, int64_t* out) {
  auto v = parse_int64(buf.substr(start, crlf - start));
  if (!v) return false;
  *out = *v;
  return true;
}

}  // namespace

RespParser::Status RespParser::parse(std::string_view buf, size_t& pos, std::vector<std::string_view>& args,
                                     std::string& error) {
  args.clear();
  if (pos >= buf.size()) return Status::Incomplete;

  if (buf[pos] != '*') {
    size_t nl = buf.find('\n', pos);
    if (nl == std::string_view::npos) {
      if (buf.size() - pos > kMaxInlineSize) {
        error = "ERR Protocol error: too big inline request";
        return Status::Error;
      }
      return Status::Incomplete;
    }
    std::string_view line = buf.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    args = split_ws(line);
    pos = nl + 1;
    return Status::Ok;
  }

  size_t crlf = buf.find("\r\n", pos);
  if (crlf == std::string_view::npos) {
    if (buf.size() - pos > kMaxInlineSize) {
      error = "ERR Protocol error: too big multibulk header";
      return Status::Error;
    }
    return Status::Incomplete;
  }
  int64_t n;
  if (!parse_len(buf, pos + 1, crlf, &n) || n > kMaxArgs) {
    error = "ERR Protocol error: invalid multibulk length";
    return Status::Error;
  }
  size_t p = crlf + 2;
  if (n <= 0) {
    pos = p;
    return Status::Ok;
  }
  args.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    if (p >= buf.size()) return Status::Incomplete;
    if (buf[p] != '$') {
      error = std::format("ERR Protocol error: expected '$', got '{}'", buf[p]);
      return Status::Error;
    }
    size_t lcrlf = buf.find("\r\n", p);
    if (lcrlf == std::string_view::npos) {
      if (buf.size() - p > 32) {
        error = "ERR Protocol error: invalid bulk length";
        return Status::Error;
      }
      return Status::Incomplete;
    }
    int64_t len;
    if (!parse_len(buf, p + 1, lcrlf, &len) || len < 0 || len > kMaxBulkSize) {
      error = "ERR Protocol error: invalid bulk length";
      return Status::Error;
    }
    size_t data = lcrlf + 2;
    if (buf.size() < data + static_cast<size_t>(len) + 2) return Status::Incomplete;
    if (buf[data + len] != '\r' || buf[data + len + 1] != '\n') {
      error = "ERR Protocol error: bulk string not terminated by CRLF";
      return Status::Error;
    }
    args.push_back(buf.substr(data, static_cast<size_t>(len)));
    p = data + static_cast<size_t>(len) + 2;
  }
  pos = p;
  return Status::Ok;
}

void Reply::header(char type, int64_t n) {
  char buf[24];
  auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), n);
  out_.push_back(type);
  out_.append(buf, end);
  out_.append("\r\n");
}

void Reply::simple(std::string_view s) {
  out_.push_back('+');
  out_.append(s);
  out_.append("\r\n");
}

void Reply::error(std::string_view msg) {
  out_.push_back('-');
  for (char c : msg) out_.push_back(c == '\r' || c == '\n' ? ' ' : c);
  out_.append("\r\n");
}

void Reply::integer(int64_t v) { header(':', v); }

void Reply::bulk(std::string_view s) {
  header('$', static_cast<int64_t>(s.size()));
  out_.append(s);
  out_.append("\r\n");
}

void Reply::null() { out_.append(proto_ >= 3 ? "_\r\n" : "$-1\r\n"); }

void Reply::null_array() { out_.append(proto_ >= 3 ? "_\r\n" : "*-1\r\n"); }

void Reply::array(size_t n) { header('*', static_cast<int64_t>(n)); }

void Reply::map(size_t n) {
  if (proto_ >= 3) header('%', static_cast<int64_t>(n));
  else header('*', static_cast<int64_t>(n * 2));
}

void Reply::set(size_t n) { header(proto_ >= 3 ? '~' : '*', static_cast<int64_t>(n)); }

void Reply::push(size_t n) { header(proto_ >= 3 ? '>' : '*', static_cast<int64_t>(n)); }

void Reply::double_value(double v) {
  std::string s = format_double(v);
  if (proto_ >= 3) {
    out_.push_back(',');
    out_.append(s);
    out_.append("\r\n");
  } else {
    bulk(s);
  }
}

}  // namespace datadam
