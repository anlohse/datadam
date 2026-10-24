#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "index/index_def.h"
#include "index/index_query.h"

namespace datadam {

// A parsed FT.SEARCH query (RediSearch syntax, TAG and NUMERIC fields):
//   *                        every document of the index
//   @f:{a | b | pre*}        a TAG field with one of these values (or prefixes)
//   @f:[lo hi]               a NUMERIC field in a range; (lo for exclusive,
//                            -inf / +inf for open ends
//   a b                      both (intersection)
//   a | b                    either (union; binds tighter than intersection)
//   -a                       not a
//   ( ... )                  grouping
// $name stands for a PARAMS value, in tags and range bounds.
struct SearchNode {
  enum class Kind { All, Tag, Range, And, Or, Not };
  Kind kind = Kind::All;
  size_t field = 0;  // Tag, Range
  // Tag: the values (normalized like the field's), and which are prefixes.
  std::vector<std::string> values;
  std::vector<bool> prefix;
  // Range
  double lo = -std::numeric_limits<double>::infinity();
  double hi = std::numeric_limits<double>::infinity();
  bool lo_open = false, hi_open = false;
  std::vector<SearchNode> children;  // And, Or, Not (one)
};

using SearchParams = std::map<std::string, std::string, std::less<>>;

// Parses a query for a search index; nullopt with an error message.
std::optional<SearchNode> parse_search_query(std::string_view text, const IndexDef& def, const SearchParams& params,
                                             std::string* error);

struct SearchOptions {
  int64_t offset = 0;
  int64_t limit = 10;
  bool no_content = false;
  // RETURN: a field name (or JSON path) and the name to show it under.
  std::vector<std::pair<std::string, std::string>> return_fields;
  std::optional<size_t> sort_field;  // SORTBY: a field of the index
  bool sort_desc = false;
  // If set, only keys that pass it count (ACL key permissions).
  std::function<bool(std::string_view key)> key_filter;
};

struct SearchResult {
  uint64_t total = 0;  // documents that match, before LIMIT
  std::vector<IndexHit> hits;
};

}  // namespace datadam
