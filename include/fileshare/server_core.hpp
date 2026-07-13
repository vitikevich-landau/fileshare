#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fileshare/client_registry.hpp"
#include "fileshare/config.hpp"

namespace fileshare {

struct ServerStatus {
    std::uint64_t uptime_seconds = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t completed_downloads = 0;
    std::size_t   active_connections = 0;
    std::size_t   shared_files = 0;
};

// Shared, I/O-independent server state: the file catalog, the client registry,
// stats, the admin command queue, and the thread-safe admin surface. Both the
// M2 thread-per-connection Server and the M4 EpollServer embed one of these, so
// the "what the server does" logic lives in exactly one place; only the "how
// bytes move" (blocking threads vs epoll) differs between them.
class ServerCore {
public:
    ServerCore(Catalog catalog, std::string config_path)
        : catalog_(std::move(catalog)), config_path_(std::move(config_path)) {}

    // --- Lifecycle ----------------------------------------------------------
    void mark_started() noexcept {
        start_time_ = std::chrono::steady_clock::now();
        running_.store(true);
    }
    void request_stop() noexcept { running_.store(false); }
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    // --- Shared state for the I/O engine ------------------------------------
    [[nodiscard]] ClientRegistry& registry() noexcept { return registry_; }

    // Encoded LIST_RESPONSE frame for the current catalog (locks internally).
    [[nodiscard]] std::vector<std::uint8_t> build_list_response() const;
    // Copy of the catalog entry for `alias`, or nullopt (locks internally).
    [[nodiscard]] std::optional<SharedFileEntry> resolve_download(const std::string& alias) const;

    void add_bytes(std::uint64_t n) noexcept { bytes_sent_.fetch_add(n); }
    void inc_completed() noexcept { completed_.fetch_add(1); }

    // --- Admin surface (all thread-safe) ------------------------------------
    AddOutcome                   admin_add(const std::string& path,
                                           const std::optional<std::string>& alias);
    bool                         admin_remove(const std::string& alias);
    [[nodiscard]] std::vector<SharedFileEntry> admin_list_files() const;
    [[nodiscard]] std::vector<ClientSnapshot>  admin_list_clients() const;
    bool                         admin_kick(std::uint64_t id);
    [[nodiscard]] ServerStatus   admin_status() const;

    // Enqueue a raw admin command line; the I/O engine drains the queue.
    void submit_command(std::string line);
    void drain_commands();

    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_.load(); }
    [[nodiscard]] std::uint64_t completed_downloads() const noexcept { return completed_.load(); }
    [[nodiscard]] std::size_t   client_count() const { return registry_.size(); }

private:
    void execute_command(const std::string& line);

    Catalog            catalog_;
    std::string        config_path_;
    mutable std::mutex catalog_mutex_;
    ClientRegistry     registry_;

    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<bool>          running_{false};
    std::chrono::steady_clock::time_point start_time_{};

    std::mutex              cmd_mutex_;
    std::deque<std::string> cmd_queue_;
};

} // namespace fileshare
