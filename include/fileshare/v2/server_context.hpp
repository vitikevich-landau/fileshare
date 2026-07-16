#pragma once

// Shared, I/O-independent server state: the VFS, the (immutable in M7) settings,
// the session registry, aggregate stats and the lifecycle flag. The transport
// (thread-per-connection now; epoll later) embeds one of these, so "what the
// server does" stays in one place -- the same separation that let v1 host three
// engines on one ServerCore.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "fileshare/v2/auth.hpp"
#include "fileshare/v2/protocol.hpp"
#include "fileshare/v2/rate_limiter.hpp"
#include "fileshare/v2/session.hpp"
#include "fileshare/v2/settings.hpp"
#include "fileshare/v2/settings_hub.hpp"
#include "fileshare/v2/vfs.hpp"

namespace fileshare::v2 {

inline constexpr const char* SERVER_VERSION = "2.0.0";

class ServerContext {
public:
    // Constructs the VFS from settings.share_root and loads the checksum cache.
    // Throws FsError if the share root cannot be resolved.
    ServerContext(Settings settings, std::string config_path);

    [[nodiscard]] Vfs&             vfs() noexcept { return *vfs_; }
    // Hot-path config read: a snapshot shared_ptr (one atomic load). Callers use
    // `->`; a live change is picked up on the next call to settings().
    [[nodiscard]] std::shared_ptr<const Settings> settings() const { return hub_.current(); }
    [[nodiscard]] SettingsHub&     settings_hub() noexcept { return hub_; }
    [[nodiscard]] RateLimiter&     rate_limiter() noexcept { return rate_limiter_; }
    [[nodiscard]] SessionRegistry& sessions() noexcept { return sessions_; }
    [[nodiscard]] const std::string& config_path() const noexcept { return config_path_; }

    // Reload the config file from disk (SIGHUP): re-reads users and applies the
    // file's settings through the hub (so hot fields update; restart-only fields
    // are ignored until a real restart). Returns "" or an error string.
    std::string reload_config();
    void request_reload() noexcept { reload_requested_.store(true); }
    [[nodiscard]] bool take_reload() noexcept { return reload_requested_.exchange(false); }

    // --- Authentication -----------------------------------------------------
    // True if any users are configured (=> challenge auth required; otherwise a
    // fresh deployment runs in no-auth bootstrap mode granting admin).
    [[nodiscard]] bool auth_required() const;
    [[nodiscard]] std::optional<User> find_user(const std::string& login) const;
    void reload_users();
    [[nodiscard]] AuthGuard& auth_guard() noexcept { return auth_guard_; }
    [[nodiscard]] std::uint32_t pbkdf2_iters() const { return settings()->auth_pbkdf2_iters; }

    // --- Lifecycle ----------------------------------------------------------
    void mark_started() noexcept {
        start_time_ = std::chrono::steady_clock::now();
        accepting_.store(true);
    }
    void request_stop() noexcept { accepting_.store(false); }
    [[nodiscard]] bool accepting() const noexcept { return accepting_.load(); }

    // --- Stats --------------------------------------------------------------
    void add_bytes(std::uint64_t n) noexcept { bytes_sent_.fetch_add(n); }
    void inc_completed() noexcept { completed_.fetch_add(1); }
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_.load(); }
    [[nodiscard]] std::uint64_t completed() const noexcept { return completed_.load(); }
    [[nodiscard]] std::uint64_t uptime_seconds() const;

    [[nodiscard]] std::uint32_t next_transfer_id() noexcept { return next_transfer_.fetch_add(1); }

    // --- Connection-handler lifetime tracking -------------------------------
    // handler_started() is called by the accept thread BEFORE it detaches a
    // handler (so the count is live before the thread runs); handler_finished()
    // by the handler as it exits. wait_for_handlers() blocks until every handler
    // has returned, so serve() never lets a detached thread outlive this object.
    void handler_started() noexcept { active_handlers_.fetch_add(1); }
    void handler_finished() noexcept {
        // Notify UNDER the mutex so the waiter cannot return (and destroy this
        // condvar) between the decrement and the notify -- a classic teardown
        // race flagged by TSan otherwise.
        std::lock_guard<std::mutex> lk(handler_mu_);
        if (active_handlers_.fetch_sub(1) == 1) {
            handler_cv_.notify_all();
        }
    }
    void wait_for_handlers() {
        std::unique_lock<std::mutex> lk(handler_mu_);
        handler_cv_.wait(lk, [this] { return active_handlers_.load() == 0; });
    }
    // Live connection count, updated synchronously on the (single) accept thread
    // before each detach -- unlike sessions().size(), which lags until the worker
    // registers, so it gives an exact cap with no accept-burst overshoot.
    [[nodiscard]] int active_handlers() const noexcept { return active_handlers_.load(); }

    // Snapshot for ADMIN_STATS.
    [[nodiscard]] AdminStats stats_snapshot() const;

    // Persist the checksum cache (called on graceful shutdown).
    void save_cache() const;

private:
    SettingsHub           hub_;
    RateLimiter           rate_limiter_;
    std::string           config_path_;
    std::unique_ptr<Vfs>  vfs_;
    SessionRegistry       sessions_;
    std::atomic<bool>     reload_requested_{false};

    mutable std::mutex    users_mutex_;
    UserDb                users_;
    AuthGuard             auth_guard_;

    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint32_t> next_transfer_{1};
    std::atomic<bool>          accepting_{false};
    std::chrono::steady_clock::time_point start_time_{};

    std::atomic<int>           active_handlers_{0};
    std::mutex                 handler_mu_;
    std::condition_variable    handler_cv_;
};

} // namespace fileshare::v2
