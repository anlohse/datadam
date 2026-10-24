#pragma once

#include <string>
#include <string_view>

namespace datadam {

// SHA-256 of `data` as 64 lowercase hex digits (how ACL passwords are kept).
std::string sha256_hex(std::string_view data);

}  // namespace datadam
