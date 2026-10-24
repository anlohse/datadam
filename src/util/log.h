#pragma once

#include <format>
#include <string_view>

namespace datadam {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_log_level(LogLevel level);
bool parse_log_level(std::string_view s, LogLevel* out);
bool log_enabled(LogLevel level);
void log_write(LogLevel level, std::string_view msg);

template <class... Args>
void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
  if (log_enabled(level)) log_write(level, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void log_debug(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
}
template <class... Args>
void log_info(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::Info, fmt, std::forward<Args>(args)...);
}
template <class... Args>
void log_warn(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::Warn, fmt, std::forward<Args>(args)...);
}
template <class... Args>
void log_error(std::format_string<Args...> fmt, Args&&... args) {
  log(LogLevel::Error, fmt, std::forward<Args>(args)...);
}

}  // namespace datadam
