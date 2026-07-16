#include "fileshare/v2/server_context.hpp"

#include <filesystem>

#include "fileshare/v2/log.hpp"

namespace fs = std::filesystem;

namespace fileshare::v2 {

ServerContext::ServerContext(Settings settings, std::string config_path)
    : settings_(std::move(settings)), config_path_(std::move(config_path)) {
    vfs_ = std::make_unique<Vfs>(fs::path(settings_.share_root));
    if (!settings_.checksum_cache_file.empty()) {
        vfs_->load_cache(fs::path(settings_.checksum_cache_file));
    }
    if (!settings_.users_file.empty()) {
        users_ = UserDb::load(settings_.users_file);
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
    if (settings_.users_file.empty()) return;
    UserDb fresh = UserDb::load(settings_.users_file);
    std::lock_guard<std::mutex> lk(users_mutex_);
    users_ = std::move(fresh);
}

std::uint64_t ServerContext::uptime_seconds() const {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
}

AdminStats ServerContext::stats_snapshot() const {
    AdminStats s;
    s.uptime_seconds      = uptime_seconds();
    s.bytes_sent          = bytes_sent_.load();
    s.completed_downloads = completed_.load();
    s.active_connections  = sessions_.size();
    s.active_downloads    = sessions_.downloading_count();
    s.shared_files        = 0;   // populated lazily; a full tree walk is avoided here
    s.per_client_bps      = settings_.limits.per_client_bps;
    s.global_bps          = settings_.limits.global_bps;
    s.version             = SERVER_VERSION;
    return s;
}

void ServerContext::save_cache() const {
    if (!settings_.checksum_cache_file.empty()) {
        try {
            vfs_->save_cache(fs::path(settings_.checksum_cache_file));
        } catch (const std::exception& e) {
            log_warn(std::string("could not persist checksum cache: ") + e.what());
        }
    }
}

} // namespace fileshare::v2
