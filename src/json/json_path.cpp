#include "json/json_path.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <set>
#include <unordered_set>

namespace datadam {

// A filter expression: ?(@.price < 10 && @.tag == "x"), !(@.a), @.b (exists).
struct JsonFilter {
  enum class Kind { Or, And, Not, Compare, Exists, Literal, Path };
  Kind kind = Kind::Literal;
  std::string op;                         // Compare: == != < <= > >=
  std::shared_ptr<const JsonFilter> a, b;  // operands
  Json literal;
  bool relative = true;  // Path: @ (the current node) or $ (the document)
  JsonPath path;
};

namespace {

using Match = JsonPath::Match;

class Parser {
 public:
  Parser(std::string_view text, std::string* error) : s_(text), error_(error) {}

  bool fail(std::string_view why) {
    if (error_->empty()) *error_ = std::format("ERR invalid path '{}': {}", s_, why);
    return false;
  }
  bool done() const { return pos_ >= s_.size(); }
  char peek(size_t ahead = 0) const { return pos_ + ahead < s_.size() ? s_[pos_ + ahead] : '\0'; }
  void skip_spaces() {
    while (!done() && (peek() == ' ' || peek() == '\t')) ++pos_;
  }
  bool eat(std::string_view token) {
    skip_spaces();
    if (s_.substr(pos_, token.size()) != token) return false;
    pos_ += token.size();
    return true;
  }

  // Steps until the text ends or, in a filter, until something that cannot
  // continue a path (an operator, a parenthesis...).
  bool steps(std::vector<JsonStep>& out, bool in_filter) {
    while (!done()) {
      JsonStep step;
      if (peek() == '.' && peek(1) == '.') {
        pos_ += 2;
        step.descendants = true;
        if (peek() == '[') {
          if (!bracket(step)) return false;
        } else if (!dotted(step, in_filter)) {
          return false;
        }
      } else if (peek() == '.') {
        ++pos_;
        if (!dotted(step, in_filter)) return false;
      } else if (peek() == '[') {
        if (!bracket(step)) return false;
      } else if (in_filter) {
        return true;
      } else {
        return fail(std::format("unexpected '{}'", peek()));
      }
      out.push_back(std::move(step));
    }
    return true;
  }

  // .name or .*
  bool dotted(JsonStep& step, bool in_filter) {
    JsonSelector sel;
    if (peek() == '*') {
      ++pos_;
      sel.kind = JsonSelector::Kind::Wildcard;
    } else {
      size_t start = pos_;
      while (!done() && peek() != '.' && peek() != '[' && peek() != ']' && !(in_filter && ends_path(peek()))) ++pos_;
      if (pos_ == start) return fail("missing member name");
      sel.name = std::string(s_.substr(start, pos_ - start));
    }
    step.selectors.push_back(std::move(sel));
    return true;
  }

  static bool ends_path(char c) { return std::string_view(" \t=!<>&|),]").find(c) != std::string_view::npos; }

  // [sel, sel, ...]
  bool bracket(JsonStep& step) {
    ++pos_;  // [
    for (;;) {
      skip_spaces();
      JsonSelector sel;
      char c = peek();
      if (c == '*') {
        ++pos_;
        sel.kind = JsonSelector::Kind::Wildcard;
      } else if (c == '\'' || c == '"') {
        if (!quoted(&sel.name)) return false;
      } else if (c == '?') {
        ++pos_;
        sel.kind = JsonSelector::Kind::Filter;
        auto f = disjunction();
        if (!f) return false;
        sel.filter = std::move(f);
      } else if (c == '-' || c == ':' || (c >= '0' && c <= '9')) {
        if (!index_or_slice(sel)) return false;
      } else {
        return fail("bad selector");
      }
      step.selectors.push_back(std::move(sel));
      skip_spaces();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ']'");
    }
  }

  bool quoted(std::string* out) {
    char q = peek();
    ++pos_;
    while (!done() && peek() != q) {
      if (peek() == '\\' && pos_ + 1 < s_.size()) ++pos_;
      out->push_back(peek());
      ++pos_;
    }
    if (done()) return fail("unterminated string");
    ++pos_;
    return true;
  }

  std::optional<int64_t> integer() {
    skip_spaces();
    size_t start = pos_;
    if (peek() == '-') ++pos_;
    while (!done() && peek() >= '0' && peek() <= '9') ++pos_;
    if (pos_ == start || (pos_ == start + 1 && s_[start] == '-')) {
      pos_ = start;
      return std::nullopt;
    }
    int64_t v = 0;
    auto r = std::from_chars(s_.data() + start, s_.data() + pos_, v);
    if (r.ec != std::errc()) return std::nullopt;
    return v;
  }

  bool index_or_slice(JsonSelector& sel) {
    auto first = integer();
    skip_spaces();
    if (peek() != ':') {
      if (!first) return fail("bad index");
      sel.kind = JsonSelector::Kind::Index;
      sel.index = *first;
      return true;
    }
    sel.kind = JsonSelector::Kind::Slice;
    sel.start = first;
    ++pos_;  // :
    sel.end = integer();
    skip_spaces();
    if (peek() == ':') {
      ++pos_;
      if (auto step = integer()) sel.step = *step;
    }
    return true;
  }

  // ---- filters
  using Node = std::shared_ptr<const JsonFilter>;

  // Filters are parsed, tested and freed recursively: their nesting and size
  // are bounded, so a crafted path cannot overflow the stack.
  static constexpr int kMaxFilterDepth = 64;
  static constexpr int kMaxFilterNodes = 1024;

  struct Nested {
    Parser& p;
    explicit Nested(Parser& parser) : p(parser) { ++p.depth_; }
    ~Nested() { --p.depth_; }
  };
  bool too_deep() {
    if (depth_ <= kMaxFilterDepth) return false;
    fail(std::format("filters nest deeper than {} levels", kMaxFilterDepth));
    return true;
  }
  bool too_many_nodes() {
    if (++nodes_ <= kMaxFilterNodes) return false;
    fail(std::format("filters have more than {} terms", kMaxFilterNodes));
    return true;
  }

  Node binary(JsonFilter::Kind kind, Node a, Node b) {
    if (too_many_nodes()) return nullptr;
    auto n = std::make_shared<JsonFilter>();
    n->kind = kind;
    n->a = std::move(a);
    n->b = std::move(b);
    return n;
  }

  Node disjunction() {
    Nested nested(*this);  // ( ... ) and @[?...] inside a filter come back here
    if (too_deep()) return nullptr;
    Node left = conjunction();
    while (left && eat("||")) {
      Node right = conjunction();
      if (!right) return nullptr;
      left = binary(JsonFilter::Kind::Or, left, right);
    }
    return left;
  }

  Node conjunction() {
    Node left = unary();
    while (left && eat("&&")) {
      Node right = unary();
      if (!right) return nullptr;
      left = binary(JsonFilter::Kind::And, left, right);
    }
    return left;
  }

  Node unary() {
    skip_spaces();
    if (peek() == '!' && peek(1) != '=') {
      ++pos_;
      Nested nested(*this);
      if (too_deep()) return nullptr;
      Node inner = unary();
      return inner ? binary(JsonFilter::Kind::Not, inner, nullptr) : nullptr;
    }
    if (peek() == '(') {
      ++pos_;
      Node inner = disjunction();
      if (!inner || !eat(")")) {
        fail("expected ')'");
        return nullptr;
      }
      return inner;
    }
    Node left = operand();
    if (!left) return nullptr;
    skip_spaces();
    for (std::string_view op : {"==", "!=", "<=", ">=", "<", ">"}) {
      if (s_.substr(pos_, op.size()) == op) {
        pos_ += op.size();
        Node right = operand();
        if (!right || too_many_nodes()) return nullptr;
        auto n = std::make_shared<JsonFilter>();
        n->kind = JsonFilter::Kind::Compare;
        n->op = std::string(op);
        n->a = left;
        n->b = right;
        return n;
      }
    }
    if (left->kind != JsonFilter::Kind::Path) {
      fail("a filter needs a comparison or a path");
      return nullptr;
    }
    auto n = std::make_shared<JsonFilter>(*left);
    n->kind = JsonFilter::Kind::Exists;
    return n;
  }

  Node operand() {
    skip_spaces();
    if (too_many_nodes()) return nullptr;
    auto n = std::make_shared<JsonFilter>();
    char c = peek();
    if (c == '@' || c == '$') {
      ++pos_;
      n->kind = JsonFilter::Kind::Path;
      n->relative = c == '@';
      std::vector<JsonStep> steps;
      if (!this->steps(steps, true)) return nullptr;
      std::string ignored;
      n->path = *JsonPath::parse("$", &ignored);
      set_steps(n->path, std::move(steps));
      return n;
    }
    n->kind = JsonFilter::Kind::Literal;
    if (c == '\'' || c == '"') {
      std::string text;
      if (!quoted(&text)) return nullptr;
      n->literal = text;
      return n;
    }
    for (auto [word, value] : {std::pair{"true", Json(true)}, {"false", Json(false)}, {"null", Json(nullptr)}}) {
      if (s_.substr(pos_, std::string_view(word).size()) == word) {
        pos_ += std::string_view(word).size();
        n->literal = value;
        return n;
      }
    }
    size_t start = pos_;
    while (!done() && std::string_view("+-.eE0123456789").find(peek()) != std::string_view::npos) ++pos_;
    std::string ignored;
    auto number = pos_ > start ? parse_json(s_.substr(start, pos_ - start), &ignored) : std::nullopt;
    if (!number || !number->is_number()) {
      fail("bad value in filter");
      return nullptr;
    }
    n->literal = *number;
    return n;
  }

  static void set_steps(JsonPath& path, std::vector<JsonStep> steps);

 private:
  std::string_view s_;
  size_t pos_ = 0;
  std::string* error_;
  int depth_ = 0;  // filters being parsed inside one another
  int nodes_ = 0;  // filter terms so far

 public:
  size_t& pos() { return pos_; }
};

}  // namespace

// Parser needs to fill a JsonPath's private steps.
struct JsonPathAccess {
  static std::vector<JsonStep>& steps(JsonPath& p) { return p.steps_; }
};

namespace {

void Parser::set_steps(JsonPath& path, std::vector<JsonStep> steps) { JsonPathAccess::steps(path) = std::move(steps); }

// ---- evaluation

void child(const Match& parent, Json& value, std::string key, size_t index, size_t ordinal, std::vector<Match>& out) {
  Match m{&value, parent.value, std::move(key), index, parent.where};
  m.where.push_back(ordinal);
  out.push_back(std::move(m));
}

// The children of a node, in order.
void children(const Match& m, std::vector<Match>& out) {
  Json& v = *m.value;
  size_t i = 0;
  if (v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it, ++i) child(m, it.value(), it.key(), 0, i, out);
  } else if (v.is_array()) {
    for (auto& e : v) {
      child(m, e, {}, i, i, out);
      ++i;
    }
  }
}

bool test(const JsonFilter& f, Json& current, Json& root);

const Json* operand_value(const JsonFilter& f, Json& current, Json& root) {
  if (f.kind == JsonFilter::Kind::Literal) return &f.literal;
  auto found = f.path.select_from(f.relative ? current : root, root);
  return found.empty() ? nullptr : found.front().value;
}

bool compare(std::string_view op, const Json* l, const Json* r) {
  if (!l || !r) {
    bool both_missing = !l && !r;
    return op == "==" ? both_missing : op == "!=" ? !both_missing : false;
  }
  int order = 0;
  bool ordered = true;
  if (l->is_number() && r->is_number()) {
    double x = l->get<double>(), y = r->get<double>();
    order = x < y ? -1 : x > y ? 1 : 0;
  } else if (l->is_string() && r->is_string()) {
    const auto& x = l->get_ref<const std::string&>();
    const auto& y = r->get_ref<const std::string&>();
    order = x < y ? -1 : x > y ? 1 : 0;
  } else {
    ordered = false;
    order = *l == *r ? 0 : 1;
  }
  if (op == "==") return order == 0;
  if (op == "!=") return order != 0;
  if (!ordered) return false;
  if (op == "<") return order < 0;
  if (op == "<=") return order <= 0;
  if (op == ">") return order > 0;
  return order >= 0;  // >=
}

bool test(const JsonFilter& f, Json& current, Json& root) {
  switch (f.kind) {
    case JsonFilter::Kind::Or: return test(*f.a, current, root) || test(*f.b, current, root);
    case JsonFilter::Kind::And: return test(*f.a, current, root) && test(*f.b, current, root);
    case JsonFilter::Kind::Not: return !test(*f.a, current, root);
    case JsonFilter::Kind::Exists: return !f.path.select_from(f.relative ? current : root, root).empty();
    case JsonFilter::Kind::Compare:
      return compare(f.op, operand_value(*f.a, current, root), operand_value(*f.b, current, root));
    case JsonFilter::Kind::Literal: return f.literal.is_boolean() && f.literal.get<bool>();
    case JsonFilter::Kind::Path: return !f.path.select_from(f.relative ? current : root, root).empty();
  }
  return false;
}

void apply(const JsonSelector& sel, const Match& m, Json& root, std::vector<Match>& out) {
  Json& v = *m.value;
  switch (sel.kind) {
    case JsonSelector::Kind::Name: {
      if (!v.is_object()) return;
      auto it = v.find(sel.name);
      if (it == v.end()) return;
      child(m, it.value(), sel.name, 0, static_cast<size_t>(std::distance(v.begin(), it)), out);
      return;
    }
    case JsonSelector::Kind::Index: {
      if (!v.is_array()) return;
      int64_t n = static_cast<int64_t>(v.size()), i = sel.index < 0 ? sel.index + n : sel.index;
      if (i < 0 || i >= n) return;
      child(m, v[static_cast<size_t>(i)], {}, static_cast<size_t>(i), static_cast<size_t>(i), out);
      return;
    }
    case JsonSelector::Kind::Wildcard: children(m, out); return;
    case JsonSelector::Kind::Slice: {
      if (!v.is_array() || sel.step == 0) return;
      int64_t n = static_cast<int64_t>(v.size()), step = sel.step;
      auto norm = [n](int64_t i) { return i < 0 ? i + n : i; };
      if (step > 0) {
        int64_t lo = std::clamp<int64_t>(norm(sel.start.value_or(0)), 0, n);
        int64_t hi = std::clamp<int64_t>(norm(sel.end.value_or(n)), 0, n);
        for (int64_t i = lo; i < hi; i = step < hi - i ? i + step : hi) {  // never past hi: no overflow
          child(m, v[static_cast<size_t>(i)], {}, static_cast<size_t>(i), static_cast<size_t>(i), out);
        }
      } else {
        int64_t hi = std::clamp<int64_t>(sel.start ? norm(*sel.start) : n - 1, -1, n - 1);
        int64_t lo = std::clamp<int64_t>(sel.end ? norm(*sel.end) : -1, -1, n - 1);
        for (int64_t i = hi; i > lo; i = step > lo - i ? i + step : lo) {
          child(m, v[static_cast<size_t>(i)], {}, static_cast<size_t>(i), static_cast<size_t>(i), out);
        }
      }
      return;
    }
    case JsonSelector::Kind::Filter: {
      std::vector<Match> kids;
      children(m, kids);
      for (auto& k : kids) {
        if (test(*sel.filter, *k.value, root)) out.push_back(std::move(k));
      }
      return;
    }
  }
}

// The node and everything below it, in document order, but not what `seen`
// holds (and adds what it returns): the nodes of one step overlap when some
// are below others, and chained ..s would otherwise multiply them.
std::vector<Match> self_and_descendants(const Match& m, std::unordered_set<const Json*>& seen) {
  std::vector<Match> out, stack{m};
  while (!stack.empty()) {
    Match cur = std::move(stack.back());
    stack.pop_back();
    if (!seen.insert(cur.value).second) continue;  // it and all below it are out already
    std::vector<Match> kids;
    children(cur, kids);
    out.push_back(std::move(cur));
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(std::move(*it));
  }
  return out;
}

void sort_for_changes(std::vector<Match>& matches) {
  // Later and deeper first: changing a value never moves one still to change.
  std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.where > b.where; });
  matches.erase(std::unique(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.where == b.where; }),
                matches.end());
}

}  // namespace

std::optional<JsonPath> JsonPath::parse(std::string_view text, std::string* error) {
  JsonPath path;
  path.text_ = std::string(text);
  if (!text.empty() && text[0] == '$') {
    Parser p(text, error);
    p.pos() = 1;
    if (!p.steps(path.steps_, false)) return std::nullopt;
    return path;
  }
  // Legacy: "." is the root; "a.b" means ".a.b".
  path.legacy_ = true;
  if (text.empty() || text == ".") return path;
  std::string dotted = text[0] == '.' || text[0] == '[' ? std::string(text) : "." + std::string(text);
  Parser p(dotted, error);
  if (!p.steps(path.steps_, false)) return std::nullopt;
  for (const auto& step : path.steps_) {
    for (const auto& sel : step.selectors) {
      if (step.descendants || (sel.kind != JsonSelector::Kind::Name && sel.kind != JsonSelector::Kind::Index &&
                               sel.kind != JsonSelector::Kind::Wildcard)) {
        *error = std::format("ERR invalid path '{}': only names, indexes and * in a legacy path", text);
        return std::nullopt;
      }
    }
  }
  return path;
}

std::vector<Match> JsonPath::run(Json& doc, Json& root, size_t steps) const {
  std::vector<Match> cur{Match{&doc, nullptr, {}, 0, {}}};
  for (size_t s = 0; s < steps && !cur.empty(); ++s) {
    const JsonStep& step = steps_[s];
    std::vector<Match> next;
    std::unordered_set<const Json*> seen;
    for (const Match& m : cur) {
      if (step.descendants) {
        for (const Match& d : self_and_descendants(m, seen)) {
          for (const auto& sel : step.selectors) apply(sel, d, root, next);
        }
      } else {
        for (const auto& sel : step.selectors) apply(sel, m, root, next);
      }
      // Unions of the same index ($[0,0][0,0]...) still double at each step.
      if (next.size() > kMaxJsonPathMatches) throw JsonPathError(std::format("ERR path '{}' selects more than {} values", text_, kMaxJsonPathMatches));
    }
    cur = std::move(next);
  }
  return cur;
}

std::vector<Match> JsonPath::select_from(Json& node, Json& root) const { return run(node, root, steps_.size()); }

std::vector<Match> JsonPath::select(Json& doc) const {
  auto found = run(doc, doc, steps_.size());
  if (legacy_ && found.size() > 1) found.resize(1);
  return found;
}

std::vector<const Json*> JsonPath::select(const Json& doc) const {
  // Selecting changes nothing; the const_cast only reuses the code.
  auto found = select(const_cast<Json&>(doc));
  std::vector<const Json*> out;
  for (const auto& m : found) out.push_back(m.value);
  return out;
}

std::vector<Match> JsonPath::select_or_create(Json& doc, size_t* created) const {
  *created = 0;
  if (!steps_.empty()) {
    const JsonStep& last = steps_.back();
    if (!last.descendants && last.selectors.size() == 1 && last.selectors[0].kind == JsonSelector::Kind::Name) {
      auto parents = run(doc, doc, steps_.size() - 1);
      if (legacy_ && parents.size() > 1) parents.resize(1);
      sort_for_changes(parents);
      for (const Match& p : parents) {
        if (p.value->is_object() && !p.value->contains(last.selectors[0].name)) {
          (*p.value)[last.selectors[0].name] = nullptr;
          ++*created;
        }
      }
    }
  }
  return select(doc);  // again: adding members may have moved what was found
}

size_t JsonPath::erase(Json& doc) const {
  auto found = select(doc);
  sort_for_changes(found);
  std::set<std::vector<size_t>> selected;
  for (const Match& m : found) selected.insert(m.where);
  auto inside_another = [&](const Match& m) {
    for (size_t len = 0; len < m.where.size(); ++len) {
      if (selected.contains(std::vector<size_t>(m.where.begin(), m.where.begin() + static_cast<ptrdiff_t>(len)))) {
        return true;
      }
    }
    return false;
  };
  size_t n = 0;
  for (const Match& m : found) {
    if (!m.parent || inside_another(m)) continue;  // goes with the value that holds it
    if (m.parent->is_object()) n += m.parent->erase(m.key);
    else if (m.parent->is_array() && m.index < m.parent->size()) {
      m.parent->erase(m.index);
      ++n;
    }
  }
  return n;
}

}  // namespace datadam
