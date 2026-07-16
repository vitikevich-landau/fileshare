#include "fileshare/v2/settings.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace fileshare::v2 {

namespace {

// Fetch j[key] as T, falling back to `def` when absent or the wrong type.
template <typename T>
T get_or(const json& j, const char* key, T def) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) {
        try { return it->get<T>(); } catch (const std::exception&) { return def; }
    }
    return def;
}

Limits limits_from(const json& j) {
    Limits l;
    if (auto it = j.find("limits"); it != j.end() && it->is_object()) {
        const json& lj = *it;
        l.max_connections       = get_or<std::uint64_t>(lj, "max_connections", l.max_connections);
        l.max_sessions_per_user = get_or<std::uint32_t>(lj, "max_sessions_per_user", l.max_sessions_per_user);
        l.per_client_bps        = get_or<std::uint64_t>(lj, "per_client_bps", l.per_client_bps);
        l.global_bps            = get_or<std::uint64_t>(lj, "global_bps", l.global_bps);
        l.handshake_timeout_s   = get_or<std::uint32_t>(lj, "handshake_timeout_s", l.handshake_timeout_s);
        l.idle_timeout_s        = get_or<std::uint32_t>(lj, "idle_timeout_s", l.idle_timeout_s);
        l.auth_fail_ban_s       = get_or<std::uint32_t>(lj, "auth_fail_ban_s", l.auth_fail_ban_s);
    }
    return l;
}

json to_json(const Settings& s) {
    return json{
        {"server", {
            {"port", s.port},
            {"share_root", s.share_root},
            {"workers", s.workers},
            {"motd", s.motd},
        }},
        {"limits", {
            {"max_connections", s.limits.max_connections},
            {"max_sessions_per_user", s.limits.max_sessions_per_user},
            {"per_client_bps", s.limits.per_client_bps},
            {"global_bps", s.limits.global_bps},
            {"handshake_timeout_s", s.limits.handshake_timeout_s},
            {"idle_timeout_s", s.limits.idle_timeout_s},
            {"auth_fail_ban_s", s.limits.auth_fail_ban_s},
        }},
        {"checksum", {{"cache_file", s.checksum_cache_file}}},
        {"events", {{"enabled", s.events_enabled}, {"debounce_ms", s.events_debounce_ms}}},
        {"auth", {{"users_file", s.users_file}, {"pbkdf2_iters", s.auth_pbkdf2_iters}}},
        {"log", {{"level", s.log_level}}},
    };
}

} // namespace

Settings Settings::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return Settings{};   // missing file -> defaults (fresh deployment boots)
    }
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error("config parse error in " + path + ": " + e.what());
    }
    if (!j.is_object()) {
        throw std::runtime_error("config root must be a JSON object: " + path);
    }

    Settings s;
    const json server = j.value("server", json::object());
    s.port       = get_or<std::uint16_t>(server, "port", s.port);
    s.share_root = get_or<std::string>(server, "share_root", s.share_root);
    s.workers    = get_or<int>(server, "workers", s.workers);
    s.motd       = get_or<std::string>(server, "motd", s.motd);
    s.limits     = limits_from(j);

    const json cks = j.value("checksum", json::object());
    s.checksum_cache_file = get_or<std::string>(cks, "cache_file", s.checksum_cache_file);

    const json ev = j.value("events", json::object());
    s.events_enabled     = get_or<bool>(ev, "enabled", s.events_enabled);
    s.events_debounce_ms = get_or<std::uint32_t>(ev, "debounce_ms", s.events_debounce_ms);

    const json au = j.value("auth", json::object());
    s.users_file        = get_or<std::string>(au, "users_file", s.users_file);
    s.auth_pbkdf2_iters = get_or<std::uint32_t>(au, "pbkdf2_iters", s.auth_pbkdf2_iters);

    const json lg = j.value("log", json::object());
    s.log_level = get_or<std::string>(lg, "level", s.log_level);
    return s;
}

void Settings::save(const std::string& path) const {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot write config: " + path);
    }
    out << to_json(*this).dump(2) << "\n";
    if (!out) {
        throw std::runtime_error("write failed: " + path);
    }
}

std::string Settings::to_json_string() const {
    return to_json(*this).dump(2);
}

std::string Settings::validate() const {
    if (port == 0) {
        return "server.port must be 1..65535";
    }
    if (share_root.empty()) {
        return "server.share_root must be set";
    }
    if (limits.global_bps != 0 && limits.per_client_bps > limits.global_bps) {
        return "limits.per_client_bps must not exceed limits.global_bps";
    }
    if (limits.handshake_timeout_s == 0 || limits.idle_timeout_s == 0) {
        return "timeouts must be > 0";
    }
    return {};
}

} // namespace fileshare::v2
