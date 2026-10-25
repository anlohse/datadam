// libFuzzer target: JSON documents and JSONPath expressions, as JSON.SET /
// JSON.GET get them from clients. Input: "<path>\n<document>".

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "json/json.h"
#include "json/json_path.h"

using namespace datadam;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view in(reinterpret_cast<const char*>(data), size);
  size_t nl = in.find('\n');
  std::string_view path_text = nl == std::string_view::npos ? in : in.substr(0, nl);
  std::string_view doc_text = nl == std::string_view::npos ? std::string_view("{}") : in.substr(nl + 1);
  std::string error;
  auto doc = parse_json(doc_text, &error);
  auto path = JsonPath::parse(path_text, &error);
  if (!doc || !path) return 0;
  for (auto& m : path->select(*doc)) {
    std::string text = dump_json(*m.value);
    // What was dumped parses back.
    if (!parse_json(text, &error)) __builtin_trap();
  }
  return 0;
}
