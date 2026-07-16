#pragma once

// Daemon configuration (see docs/v2/03-server-daemon.md). A plain immutable
// value type in M7; M11 wraps it in a SettingsHub for hot reload. Fields are
// annotated hot/restart in the docs -- the struct itself is just data.

#include <cstdint>
#include <string>

namespace fileshare::v2 {

struct Limits {
    std::uint64_t max_connections      = 200;   // hot
    std::uint32_t max_sessions_per_user= 3;     // hot
    std::uint64_t per_client_bps       = 0;     // hot; 0 = unlimited
    std::uint64_t global_bps           = 0;     // hot; 0 = unlimited
    std::uint32_t handshake_timeout_s  = 10;    // hot
    std::uint32_t idle_timeout_s       = 600;   // hot
    std::uint32_t auth_fail_ban_s      = 60;    // hot
};

struct Settings {
    std::uint16_t port         = 5555;          // restart
    std::string   share_root   = "./share";     // restart
    int           workers      = 0;             // restart; 0 = hardware_concurrency
    std::string   motd;                         // hot
    Limits        limits;
    std::string   checksum_cache_file = "checksums.cache";  // restart
    bool          events_enabled      = true;   // hot
    std::uint32_t events_debounce_ms  = 500;    // hot
    std::string   users_file          = "users.json";       // restart (path)
    std::uint32_t auth_pbkdf2_iters   = 200000;              // restart (must match stored users)
    std::string   log_level           = "info"; // hot

    // Load from a JSON file. A missing file yields defaults (not an error, so a
    // fresh deployment boots). Throws std::runtime_error on malformed JSON.
    [[nodiscard]] static Settings load(const std::string& path);

    // Serialise to a JSON file. Throws std::runtime_error on write failure.
    void save(const std::string& path) const;

    // Pretty JSON of the effective settings (used by ADMIN_GET_CONFIG in M11).
    [[nodiscard]] std::string to_json_string() const;

    // Validate ranges/relationships. Returns an error string, or empty if ok.
    [[nodiscard]] std::string validate() const;
};

} // namespace fileshare::v2
