#pragma once

#include <cstdint>

namespace datadam {

// The process's resident memory in bytes (0 where unknown).
uint64_t process_rss_bytes();

}  // namespace datadam
