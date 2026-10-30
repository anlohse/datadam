#include "json/json.h"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace datadam {

namespace {

// How deep the text nests, counting brackets outside strings: a deep
// document is refused before it is built (512 MB of '[' would be tens of GB
// of nested arrays). Invalid JSON is the parser's to refuse.
bool nests_too_deep(std::string_view text) {
  int depth = 0;
  bool in_string = false;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (in_string) {
      if (c == '\\') ++i;
      else if (c == '"') in_string = false;
    } else if (c == '"') {
      in_string = true;
    } else if (c == '[' || c == '{') {
      if (++depth > kMaxJsonDepth) return true;
    } else if (c == ']' || c == '}') {
      --depth;
    }
  }
  return false;
}

}  // namespace

std::optional<Json> parse_json(std::string_view text, std::string* error) {
  if (nests_too_deep(text)) {
    *error = std::format("ERR JSON nests deeper than {} levels", kMaxJsonDepth);
    return std::nullopt;
  }
  Json v = Json::parse(text.begin(), text.end(), nullptr, false);
  if (v.is_discarded()) {
    *error = "ERR invalid JSON";
    return std::nullopt;
  }
  return v;
}

int json_depth(const Json& v) {
  int deepest = 0;
  std::vector<std::pair<const Json*, int>> stack{{&v, 0}};
  while (!stack.empty()) {
    auto [node, depth] = stack.back();
    stack.pop_back();
    if (!node->is_structured()) continue;
    deepest = std::max(deepest, depth + 1);
    if (depth + 1 > kMaxJsonDepth) return depth + 1;  // enough to refuse it
    for (const auto& child : *node) stack.emplace_back(&child, depth + 1);
  }
  return deepest;
}

namespace {

std::string scalar(const Json& v) { return v.dump(-1, ' ', false, Json::error_handler_t::replace); }

void dump(const Json& v, const JsonFormat& f, int level, std::string& out) {
  auto line = [&](int depth) {
    out += f.newline;
    for (int i = 0; i < depth; ++i) out += f.indent;
  };
  if (v.is_object() || v.is_array()) {
    bool object = v.is_object();
    out += object ? '{' : '[';
    if (v.empty()) {
      out += object ? '}' : ']';
      return;
    }
    bool first = true;
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (!first) out += ',';
      first = false;
      line(level + 1);
      if (object) {
        out += scalar(Json(it.key()));
        out += ':';
        out += f.space;
      }
      dump(*it, f, level + 1, out);
    }
    line(level);
    out += object ? '}' : ']';
    return;
  }
  out += scalar(v);
}

}  // namespace

std::string dump_json(const Json& v) { return scalar(v); }

std::string dump_json(const Json& v, const JsonFormat& format) {
  if (format.plain()) return scalar(v);
  std::string out;
  dump(v, format, 0, out);
  return out;
}

std::string_view json_type_name(const Json& v) {
  switch (v.type()) {
    case Json::value_t::object: return "object";
    case Json::value_t::array: return "array";
    case Json::value_t::string: return "string";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned: return "integer";
    case Json::value_t::number_float: return "number";
    case Json::value_t::boolean: return "boolean";
    case Json::value_t::null: return "null";
    default: return "unknown";
  }
}

}  // namespace datadam
