// libFuzzer target: the RESP request parser, which reads raw network bytes.
// It must never read outside the buffer, never stall on an Ok without
// progress, and only hand out arguments that point into the buffer.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "server/resp.h"

using namespace datadam;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view buf(reinterpret_cast<const char*>(data), size);
  size_t pos = 0;
  std::vector<std::string_view> args;
  std::string error;
  for (int i = 0; i < 10000 && pos < buf.size(); ++i) {
    size_t before = pos;
    if (RespParser::parse(buf, pos, args, error) != RespParser::Status::Ok) break;
    if (pos <= before || pos > buf.size()) __builtin_trap();  // no progress, or past the end
    for (std::string_view a : args) {
      if (a.data() < buf.data() || a.data() + a.size() > buf.data() + buf.size()) __builtin_trap();
    }
  }
  return 0;
}
