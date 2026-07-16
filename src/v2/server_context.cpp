#include "fileshare/v2/server_context.hpp"

#include <filesystem>

#include "fileshare/v2/log.hpp"

namespace fs = std::filesystem;

namespace fileshare::v2 {

ServerContext::ServerContext(Settings settings, std::string config_path)
    : hub_(std::move(settings)), config_path_(std::move(config_path)) {
    const auto s = hub_.current();
    vfs_ = std::make_unique<Vfs>(fs::path(s->share_root));
    if (!s->checksum_cache_file.empty()) {
        vfs_->load_cache(fs::path(s->checksum_cache_file));
    }
    if (!s->users_file.empty()) {
        users_ = UserDb::load(s->users_file);
        if (!users_.empty()) {
            log_info(std::to_string(users_.size()) + " user(s) loaded; challenge auth enabled");
        } else {
            log_warn("no users configured -- running in no-auth bootstrap mode (grants admin)");
        }
    }
}

bool ServerContext::auth_required() const {
    std::lock_guard<std::mutex> lk(users_mutex_);
    return !users_.empty();
}

std::optional<User> ServerContext::find_user(const std::string& login) const {
    std::lock_guard<std::mutex> lk(users_mutex_);
    return users_.find(login);
}

void ServerContext::reload_users() {
    const auto s = hub_.current();
    if (s->users_file.empty()) return;
    UserDb fresh = UserDb::load(s->users_file);
    std::lock_guard<std::mutex> lk(users_mutex_);
    users_ = std::move(fresh);
}

std::string ServerContext::reload_config() {
    Settings fresh;
    try {
        fresh = Settings::load(config_path_.empty() ? "config.json" : config_path_);
    } catch (const std::exception& e) {
        return e.what();
    }
    // Preserve restart-only fields from the running snapshot -- they cannot
    // change without a restart, and blindly adopting the file's values would
    // misrepresent the live server.
    const auto live = hub_.current();
    fresh.port = live->port;
    fresh.share_root = live->share_root;
    fresh.workers = live->workers;
    fresh.checksum_cache_file = live->checksum_cache_file;
    fresh.auth_pbkdf2_iters = live->auth_pbkdf2_iters;

    if (const std::string err = hub_.apply(fresh); !err.empty()) {
        return err;
    }
    set_log_level(log_level_from_string(fresh.log_level));
    reload_users();
    log_info("configuration reloaded from " + config_path_);
    return {};
}

std::uint64_t ServerContext::uptime_seconds() const {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
}

AdminStats ServerContext::stats_snapshot() const {
    const auto cfg = hub_.current();
    AdminStats s;
    s.uptime_seconds      = uptime_seconds();
    s.bytes_sent          = bytes_sent_.load();
    s.completed_downloads = completed_.load();
    s.active_connections  = sessions_.size();
    s.active_downloads    = sessions_.downloading_count();
    s.shared_files        = 0;   // populated lazily; a full tree walk is avoided here
    s.per_client_bps      = cfg->limits.per_client_bps;
    s.global_bps          = cfg->limits.global_bps;
    s.version             = SERVER_VERSION;
    return s;
}

void ServerContext::save_cache() const {
    const auto cfg = hub_.current();
    if (!cfg->checksum_cache_file.empty()) {
        try {
            vfs_->save_cache(fs::path(cfg->checksum_cache_file));
        } catch (const std::exception& e) {
            log_warn(std::string("could not persist checksum cache: ") + e.what());
        }
    }
}

} // namespace fileshare::v2
