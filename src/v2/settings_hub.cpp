#include "fileshare/v2/settings_hub.hpp"

#include <array>
#include <charconv>

namespace fileshare::v2 {

namespace {

// The set of keys admins may change live. Everything else is restart-only.
constexpr std::array<const char*, 10> kHotKeys{
    "limits.per_client_bps",
    "limits.global_bps",
    "limits.max_connections",
    "limits.max_sessions_per_user",
    "limits.handshake_timeout_s",
    "limits.idle_timeout_s",
    "limits.auth_fail_ban_s",
    "server.motd",
    "log.level",
    "events.debounce_ms",
};

// Parse an unsigned integer; returns false on any garbage / overflow.
bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(begin, end, out);
    return ec == std::errc() && p == end;
}

} // namespace

bool SettingsHub::is_hot_key(const std::string& key) {
    for (const char* k : kHotKeys) {
        if (key == k) return true;
    }
    return false;
}

std::string SettingsHub::value_of(const Settings& s, const std::string& key) {
    if (key == "limits.per_client_bps")       return std::to_string(s.limits.per_client_bps);
    if (key == "limits.global_bps")           return std::to_string(s.limits.global_bps);
    if (key == "limits.max_connections")      return std::to_string(s.limits.max_connections);
    if (key == "limits.max_sessions_per_user")return std::to_string(s.limits.max_sessions_per_user);
    if (key == "limits.handshake_timeout_s")  return std::to_string(s.limits.handshake_timeout_s);
    if (key == "limits.idle_timeout_s")       return std::to_string(s.limits.idle_timeout_s);
    if (key == "limits.auth_fail_ban_s")      return std::to_string(s.limits.auth_fail_ban_s);
    if (key == "server.motd")                 return s.motd;
    if (key == "log.level")                   return s.log_level;
    if (key == "events.debounce_ms")          return std::to_string(s.events_debounce_ms);
    return {};
}

std::string SettingsHub::apply(const Settings& next) {
    if (const std::string err = next.validate(); !err.empty()) {
        return err;
    }
    snapshot_.store(std::make_shared<const Settings>(next));
    return {};
}

std::string SettingsHub::set(const std::string& key, const std::string& value,
                             std::string* old_value_out) {
    if (!is_hot_key(key)) {
        return "unknown or restart-only key: " + key;
    }
    std::lock_guard<std::mutex> lk(write_mu_);
    Settings next = *current();   // copy-on-write from the live snapshot
    if (old_value_out) *old_value_out = value_of(next, key);

    // Numeric keys.
    if (key == "server.motd") {
        next.motd = value;
    } else if (key == "log.level") {
        if (value != "debug" && value != "info" && value != "warn" && value != "error") {
            return "log.level must be debug|info|warn|error";
        }
        next.log_level = value;
    } else {
        std::uint64_t n = 0;
        if (!parse_u64(value, n)) return "value must be a non-negative integer";
        if (key == "limits.per_client_bps")            next.limits.per_client_bps = n;
        else if (key == "limits.global_bps")           next.limits.global_bps = n;
        else if (key == "limits.max_connections")      next.limits.max_connections = n;
        else if (key == "limits.max_sessions_per_user")next.limits.max_sessions_per_user = static_cast<std::uint32_t>(n);
        else if (key == "limits.handshake_timeout_s")  next.limits.handshake_timeout_s = static_cast<std::uint32_t>(n);
        else if (key == "limits.idle_timeout_s")       next.limits.idle_timeout_s = static_cast<std::uint32_t>(n);
        else if (key == "limits.auth_fail_ban_s")      next.limits.auth_fail_ban_s = static_cast<std::uint32_t>(n);
        else if (key == "events.debounce_ms")          next.events_debounce_ms = static_cast<std::uint32_t>(n);
    }

    if (const std::string err = next.validate(); !err.empty()) {
        return err;   // atomically nothing changed
    }
    snapshot_.store(std::make_shared<const Settings>(next));
    if (cb_) cb_(key, value);
    return {};
}

} // namespace fileshare::v2
