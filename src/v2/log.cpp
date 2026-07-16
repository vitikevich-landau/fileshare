#include "fileshare/v2/log.hpp"

#include <atomic>
#include <ctime>
#include <iostream>
#include <mutex>

namespace fileshare::v2 {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::INFO)};
std::mutex       g_out_mutex;

const char* level_tag(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?????";
}
} // namespace

void set_log_level(LogLevel lvl) { g_level.store(static_cast<int>(lvl)); }

LogLevel log_level_from_string(const std::string& s) noexcept {
    if (s == "debug") return LogLevel::DEBUG;
    if (s == "warn")  return LogLevel::WARN;
    if (s == "error") return LogLevel::ERROR;
    return LogLevel::INFO;
}

void log(LogLevel lvl, const std::string& msg) {
    if (static_cast<int>(lvl) < g_level.load()) {
        return;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    std::lock_guard<std::mutex> lk(g_out_mutex);
    std::cerr << ts << ' ' << level_tag(lvl) << ' ' << msg << '\n';
}

} // namespace fileshare::v2
