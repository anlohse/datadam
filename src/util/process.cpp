#include "util/process.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>

#include <fstream>
#endif

namespace datadam {

uint64_t process_rss_bytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS c{};
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c))) return 0;
  return c.WorkingSetSize;
#elif defined(__linux__)
  // statm: total and resident pages.
  std::ifstream statm("/proc/self/statm");
  uint64_t total = 0, resident = 0;
  if (!(statm >> total >> resident)) return 0;
  return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
  return 0;
#endif
}

}  // namespace datadam
