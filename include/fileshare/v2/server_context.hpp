#pragma once

// Shared, I/O-independent server state: the VFS, the (immutable in M7) settings,
// the session registry, aggregate stats and the lifecycle flag. The transport
// (thread-per-connection now; epoll later) embeds one of these, so "what the
// server does" stays in one place -- the same separation that let v1 host three
// engines on one ServerCore.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "fileshare/v2/protocol.hpp"
#include "fileshare/v2/session.hpp"
#include "fileshare/v2/settings.hpp"
#include "fileshare/v2/vfs.hpp"

namespace fileshare::v2 {

inline constexpr const char* SERVER_VERSION = "2.0.0";

class ServerContext {
public:
    // Constructs the VFS from settings.share_root and loads the checksum cache.
    // Throws FsError if the share root cannot be resolved.
    ServerContext(Settings settings, std::string config_path);

    [[nodiscard]] Vfs&             vfs() noexcept { return *vfs_; }
    [[nodiscard]] const Settings&  settings() const noexcept { return settings_; }
    [[nodiscard]] SessionRegistry& sessions() noexcept { return sessions_; }
    [[nodiscard]] const std::string& config_path() const noexcept { return config_path_; }

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

    // Snapshot for ADMIN_STATS.
    [[nodiscard]] AdminStats stats_snapshot() const;

    // Persist the checksum cache (called on graceful shutdown).
    void save_cache() const;

private:
    Settings              settings_;
    std::string           config_path_;
    std::unique_ptr<Vfs>  vfs_;
    SessionRegistry       sessions_;

    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint32_t> next_transfer_{1};
    std::atomic<bool>          accepting_{false};
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace fileshare::v2
