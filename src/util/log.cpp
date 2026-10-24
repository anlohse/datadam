#include "util/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "util/strings.h"

namespace datadam {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_mutex;
}  // namespace

void set_log_level(LogLevel level) { g_level.store(static_cast<int>(level)); }

bool parse_log_level(std::string_view s, LogLevel* out) {
  std::string l = to_lower(s);
  if (l == "debug") *out = LogLevel::Debug;
  else if (l == "info") *out = LogLevel::Info;
  else if (l == "warn" || l == "warning") *out = LogLevel::Warn;
  else if (l == "error") *out = LogLevel::Error;
  else return false;
  return true;
}

bool log_enabled(LogLevel level) { return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed); }

void log_write(LogLevel level, std::string_view msg) {
  static constexpr const char* kNames[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
  auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
  std::string line = std::format("{:%Y-%m-%d %H:%M:%S} {} {}\n", now, kNames[static_cast<int>(level)], msg);
  std::lock_guard lock(g_mutex);
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
}

}  // namespace datadam
