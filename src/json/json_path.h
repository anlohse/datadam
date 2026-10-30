#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "json/json.h"

namespace datadam {

struct JsonFilter;  // a ?(...) expression, see json_path.cpp

// The most values one path may select, at any of its steps.
inline constexpr size_t kMaxJsonPathMatches = 1 << 20;

// A path that selects more than that: the message is the reply's error.
class JsonPathError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// One selector of a path step: a member name, an array index (negative:
// from the end), *, a slice [start:end:step], or a filter ?(...).
struct JsonSelector {
  enum class Kind { Name, Index, Wildcard, Slice, Filter };
  Kind kind = Kind::Name;
  std::string name;
  int64_t index = 0;
  std::optional<int64_t> start, end;
  int64_t step = 1;
  std::shared_ptr<const JsonFilter> filter;
};

// A step applies its selectors (a union: [0,2], ['a','b']) to the current
// nodes, or with `descendants` (..) to each of them and everything below.
struct JsonStep {
  bool descendants = false;
  std::vector<JsonSelector> selectors;
};

// A path into a JSON document, in either syntax RedisJSON accepts:
//   JSONPath  $  $.a.b  $['a']  $[0]  $[-1]  $.*  $..b  $[1:3]  $[0,2]  $.c[?(@.x > 1 && @.y == "z")]
//   legacy    .  .a.b   a.b     a[0]  ["a"]
// A legacy path selects (at most) one value; RedisJSON commands then answer
// with that value, or an error when nothing matches.
class JsonPath {
 public:
  static std::optional<JsonPath> parse(std::string_view text, std::string* error);

  bool legacy() const { return legacy_; }
  bool root() const { return steps_.empty(); }
  const std::string& text() const { return text_; }

  // A selected value and where it sits, so it can be replaced or removed.
  struct Match {
    Json* value = nullptr;
    Json* parent = nullptr;     // null for the root
    std::string key;            // member name, in an object parent
    size_t index = 0;           // position, in an array parent
    std::vector<size_t> where;  // positions from the root, for document order
  };

  // The selected values, in document order (a legacy path: the first only).
  std::vector<Match> select(Json& doc) const;
  std::vector<const Json*> select(const Json& doc) const;
  // For JSON.SET: when the last step is a plain member name and an object
  // that should hold it lacks it, the member is created (null) and selected;
  // *created counts them.
  std::vector<Match> select_or_create(Json& doc, size_t* created) const;
  // Removes the selected values (never the root); returns how many.
  size_t erase(Json& doc) const;

  // Used by filters: the path's steps from a given node.
  std::vector<Match> select_from(Json& node, Json& root) const;

 private:
  friend struct JsonPathAccess;  // the parser of filter paths
  std::vector<Match> run(Json& doc, Json& root, size_t steps) const;

  bool legacy_ = false;
  std::string text_;
  std::vector<JsonStep> steps_;
};

}  // namespace datadam
