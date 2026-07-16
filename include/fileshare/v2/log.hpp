#pragma once

// Minimal leveled logger to stderr (journald / docker collect it). Timestamped,
// thread-safe. Deliberately tiny -- no external logging dependency.

#include <string>

namespace fileshare::v2 {

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

void      set_log_level(LogLevel lvl);
LogLevel  log_level_from_string(const std::string& s) noexcept;
void      log(LogLevel lvl, const std::string& msg);

inline void log_debug(const std::string& m) { log(LogLevel::DEBUG, m); }
inline void log_info (const std::string& m) { log(LogLevel::INFO,  m); }
inline void log_warn (const std::string& m) { log(LogLevel::WARN,  m); }
inline void log_error(const std::string& m) { log(LogLevel::ERROR, m); }

} // namespace fileshare::v2
