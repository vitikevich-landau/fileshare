#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fileshare/client_registry.hpp"
#include "fileshare/config.hpp"
#include "fileshare/net.hpp"
#include "fileshare/protocol.hpp"

namespace fileshare {

struct ServerStatus {
    std::uint64_t uptime_seconds = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t completed_downloads = 0;
    std::size_t   active_connections = 0;
    std::size_t   shared_files = 0;
};

// M2 server: one detached thread per connection, so many clients download at
// once. The accept loop also drains an admin command queue (fed by the console
// thread) each poll, so admin commands run on the network engine without the
// console ever blocking the network or vice-versa (§6). All admin_* methods are
// individually thread-safe and directly unit-testable.
class Server {
public:
    explicit Server(Catalog catalog, std::string config_path = {})
        : catalog_(std::move(catalog)), config_path_(std::move(config_path)) {}

    // Bind + listen; returns the actual bound port. Throws net::NetError.
    std::uint16_t listen(std::uint16_t port);

    // Accept connections (a thread each) and drain admin commands until stop().
    // Blocking; on return all connection threads have finished. Run on its own
    // thread so the caller can drive the admin console.
    void serve_forever();

    // Ask the engine to finish. Safe from any thread (also the `shutdown` cmd).
    void stop() noexcept { running_.store(false); }

    // --- Admin surface (all thread-safe) ------------------------------------
    AddOutcome                   admin_add(const std::string& path,
                                           const std::optional<std::string>& alias);
    bool                         admin_remove(const std::string& alias);
    [[nodiscard]] std::vector<SharedFileEntry> admin_list_files() const;
    [[nodiscard]] std::vector<ClientSnapshot>  admin_list_clients() const;
    bool                         admin_kick(std::uint64_t id);
    [[nodiscard]] ServerStatus   admin_status() const;

    // Enqueue a raw admin command line (console thread -> engine).
    void submit_command(std::string line);

    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_.load(); }
    [[nodiscard]] std::uint64_t completed_downloads() const noexcept { return completed_.load(); }
    [[nodiscard]] std::size_t   client_count() const { return registry_.size(); }

private:
    void handle_client(net::Socket client, std::string peer);
    void finish_connection(const std::shared_ptr<ClientEntry>& entry) noexcept;
    void handle_list(net::Socket& client);
    void handle_download(net::Socket& client, const DownloadRequest& req, ClientEntry& info);
    void drain_commands();
    void execute_command(const std::string& line);

    Catalog            catalog_;
    std::string        config_path_;
    mutable std::mutex catalog_mutex_;

    net::Socket        listener_;
    std::atomic<bool>  running_{false};
    std::chrono::steady_clock::time_point start_time_{};

    ClientRegistry     registry_;

    // Detached connection threads, drained to zero during teardown.
    std::atomic<int>        active_{0};
    std::mutex              drain_mutex_;
    std::condition_variable drain_cv_;

    std::mutex              cmd_mutex_;
    std::deque<std::string> cmd_queue_;

    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> completed_{0};
};

} // namespace fileshare
