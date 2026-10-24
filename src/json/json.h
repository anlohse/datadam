#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace datadam {

// A JSON document. Objects keep their members in insertion order, and
// integers stay integers, as in RedisJSON.
using Json = nlohmann::ordered_json;

// Documents (and values set into them) nest at most this deep.
inline constexpr int kMaxJsonDepth = 128;

// Parses text; nullopt with *error if it is not JSON or nests too deep.
std::optional<Json> parse_json(std::string_view text, std::string* error);

// How deep v nests (a scalar: 0, [1]: 1).
int json_depth(const Json& v);

// Compact text, without spaces: {"a":[1,2]}. Stored documents use it.
std::string dump_json(const Json& v);

// JSON.GET's INDENT, NEWLINE and SPACE: written before each nested level,
// after each element, and after each ':'.
struct JsonFormat {
  std::string indent, newline, space;
  bool plain() const { return indent.empty() && newline.empty() && space.empty(); }
};
std::string dump_json(const Json& v, const JsonFormat& format);

// RedisJSON's names: object, array, string, integer, number, boolean, null.
std::string_view json_type_name(const Json& v);

}  // namespace datadam
