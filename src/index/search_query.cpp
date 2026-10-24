#include "index/search_query.h"

#include <cctype>
#include <cmath>
#include <format>

#include "util/strings.h"

namespace datadam {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_param_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class QueryParser {
 public:
  QueryParser(std::string_view text, const IndexDef& def, const SearchParams& params)
      : text_(text), def_(def), params_(params) {}

  std::optional<SearchNode> parse(std::string* error) {
    auto n = parse_and();
    if (n) {
      skip_ws();
      if (!at_end()) n = fail("unexpected ')'");
    }
    if (!n) *error = std::move(error_);
    return n;
  }

 private:
  bool at_end() const { return pos_ >= text_.size(); }
  char peek() const { return at_end() ? '\0' : text_[pos_]; }
  void skip_ws() {
    while (!at_end() && is_space(text_[pos_])) ++pos_;
  }
  std::nullopt_t fail(std::string_view why) {
    if (error_.empty()) error_ = std::format("ERR Syntax error at offset {}: {}", pos_, why);
    return std::nullopt;
  }

  // Juxtaposed clauses: all of them.
  std::optional<SearchNode> parse_and() {
    std::vector<SearchNode> items;
    for (;;) {
      skip_ws();
      if (at_end() || peek() == ')') break;
      auto n = parse_or();
      if (!n) return std::nullopt;
      items.push_back(std::move(*n));
    }
    if (items.empty()) return fail("empty query");
    if (items.size() == 1) return std::move(items.front());
    SearchNode n;
    n.kind = SearchNode::Kind::And;
    n.children = std::move(items);
    return n;
  }

  std::optional<SearchNode> parse_or() {
    std::vector<SearchNode> items;
    for (;;) {
      auto n = parse_unary();
      if (!n) return std::nullopt;
      items.push_back(std::move(*n));
      skip_ws();
      if (peek() != '|') break;
      ++pos_;
    }
    if (items.size() == 1) return std::move(items.front());
    SearchNode n;
    n.kind = SearchNode::Kind::Or;
    n.children = std::move(items);
    return n;
  }

  std::optional<SearchNode> parse_unary() {
    skip_ws();
    if (peek() == '-') {
      ++pos_;
      auto c = parse_unary();
      if (!c) return std::nullopt;
      SearchNode n;
      n.kind = SearchNode::Kind::Not;
      n.children.push_back(std::move(*c));
      return n;
    }
    if (peek() == '~') {  // optional clause: scoring only, so the clause itself
      ++pos_;
      return parse_unary();
    }
    return parse_atom();
  }

  std::optional<SearchNode> parse_atom() {
    skip_ws();
    if (at_end()) return fail("expected a clause");
    char c = peek();
    if (c == '(') {
      ++pos_;
      auto n = parse_and();
      if (!n) return std::nullopt;
      skip_ws();
      if (peek() != ')') return fail("expected ')'");
      ++pos_;
      return n;
    }
    if (c == '*') {
      ++pos_;
      return SearchNode{};
    }
    if (c != '@') {
      return fail(
          "datadam has no full-text search: query TAG fields with @field:{value} and NUMERIC fields with "
          "@field:[min max]");
    }
    ++pos_;
    std::string name;
    while (!at_end() && peek() != ':' && !is_space(peek())) {
      if (peek() == '\\' && pos_ + 1 < text_.size()) ++pos_;
      name.push_back(text_[pos_++]);
    }
    if (peek() != ':') return fail("expected ':' after the field name");
    ++pos_;
    int fi = def_.field_index(name);
    if (fi < 0) return fail(std::format("unknown field '{}'", name));
    auto field = static_cast<size_t>(fi);
    bool numeric = def_.fields[field].type == IndexFieldType::Numeric;
    skip_ws();
    if (peek() == '{') {
      if (numeric) return fail(std::format("field '{}' is NUMERIC: use @{}:[min max]", name, name));
      return parse_tags(field);
    }
    if (peek() == '[') {
      if (!numeric) return fail(std::format("field '{}' is TAG: use @{}:{{value}}", name, name));
      return parse_range(field);
    }
    return fail(std::format("field '{}' takes {}", name, numeric ? "[min max]" : "{value}"));
  }

  // $name at pos_ (after the '$'): the parameter's value.
  std::optional<std::string> param() {
    size_t start = pos_;
    while (!at_end() && is_param_char(peek())) ++pos_;
    std::string_view name = text_.substr(start, pos_ - start);
    auto it = params_.find(name);
    if (it == params_.end()) {
      fail(std::format("no such parameter '{}' (give it with PARAMS)", name));
      return std::nullopt;
    }
    return it->second;
  }

  std::optional<SearchNode> parse_tags(size_t field) {
    ++pos_;  // '{'
    SearchNode n;
    n.kind = SearchNode::Kind::Tag;
    n.field = field;
    std::string cur;
    bool star = false;  // cur ends (ignoring blanks) with an unescaped '*'
    auto add = [&](char ch, bool escaped) {
      cur.push_back(ch);
      if (!is_space(ch) || escaped) star = ch == '*' && !escaped;
    };
    for (;;) {
      if (at_end()) return fail("missing '}'");
      char c = text_[pos_];
      if (c == '\\') {
        if (pos_ + 1 >= text_.size()) return fail("missing '}'");
        add(text_[pos_ + 1], true);
        pos_ += 2;
      } else if (c == '"') {
        ++pos_;
        while (!at_end() && peek() != '"') {
          if (peek() == '\\' && pos_ + 1 < text_.size()) ++pos_;
          add(text_[pos_++], true);
        }
        if (at_end()) return fail("missing '\"'");
        ++pos_;
      } else if (c == '$' && cur.find_first_not_of(" \t") == std::string::npos && pos_ + 1 < text_.size() &&
                 is_param_char(text_[pos_ + 1])) {
        ++pos_;
        auto v = param();
        if (!v) return std::nullopt;
        for (char ch : *v) add(ch, true);
      } else if (c == '|' || c == '}') {
        ++pos_;
        size_t b = cur.find_first_not_of(" \t"), e = cur.find_last_not_of(" \t");
        std::string v = b == std::string::npos ? std::string() : cur.substr(b, e - b + 1);
        bool prefix = star && !v.empty();
        if (prefix) v.pop_back();
        if (v.empty()) return fail("empty tag");
        n.values.push_back(def_.normalize(field, v));
        n.prefix.push_back(prefix);
        cur.clear();
        star = false;
        if (c == '}') return n;
      } else {
        add(c, false);
        ++pos_;
      }
    }
  }

  std::optional<double> bound(bool* open, bool upper) {
    skip_ws();
    std::string token;
    while (!at_end() && !is_space(peek()) && peek() != ',' && peek() != ']') token.push_back(text_[pos_++]);
    if (token.starts_with("$")) {
      auto it = params_.find(std::string_view(token).substr(1));
      if (it == params_.end()) {
        fail(std::format("no such parameter '{}' (give it with PARAMS)", token.substr(1)));
        return std::nullopt;
      }
      token = it->second;
    }
    *open = token.starts_with("(");
    if (*open) token.erase(0, 1);
    std::string t = to_lower(token);
    if (t == "-inf") return -std::numeric_limits<double>::infinity();
    if (t == "+inf" || t == "inf") return std::numeric_limits<double>::infinity();
    auto v = parse_double(token);
    if (!v || std::isnan(*v)) {
      fail(std::format("bad {} bound '{}'", upper ? "upper" : "lower", token));
      return std::nullopt;
    }
    return v;
  }

  std::optional<SearchNode> parse_range(size_t field) {
    ++pos_;  // '['
    SearchNode n;
    n.kind = SearchNode::Kind::Range;
    n.field = field;
    auto lo = bound(&n.lo_open, false);
    if (!lo) return std::nullopt;
    skip_ws();
    if (peek() == ',') ++pos_;
    auto hi = bound(&n.hi_open, true);
    if (!hi) return std::nullopt;
    skip_ws();
    if (peek() != ']') return fail("expected ']'");
    ++pos_;
    n.lo = *lo;
    n.hi = *hi;
    return n;
  }

  std::string_view text_;
  const IndexDef& def_;
  const SearchParams& params_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

std::optional<SearchNode> parse_search_query(std::string_view text, const IndexDef& def, const SearchParams& params,
                                             std::string* error) {
  return QueryParser(text, def, params).parse(error);
}

}  // namespace datadam
