#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fileshare/client_registry.hpp"
#include "fileshare/config.hpp"
#include "fileshare/net.hpp"
#include "fileshare/protocol.hpp"
#include "fileshare/server_core.hpp"

namespace fileshare {

// M2 server: one detached thread per connection (portable; used for Windows
// development and the tests). Shared state and the admin surface live in
// ServerCore; this class only drives blocking-socket I/O. The M4 EpollServer is
// the Linux/Docker deployment target and shares the same ServerCore contract.
class Server {
public:
    explicit Server(Catalog catalog, std::string config_path = {},
                    std::chrono::milliseconds drain_grace = std::chrono::seconds(5))
        : core_(std::move(catalog), std::move(config_path)), drain_grace_(drain_grace) {}

    std::uint16_t listen(std::uint16_t port);
    void serve_forever();
    void stop() noexcept { core_.request_stop(); }

    // --- Admin surface (delegated to ServerCore) ----------------------------
    AddOutcome admin_add(const std::string& path, const std::optional<std::string>& alias) {
        return core_.admin_add(path, alias);
    }
    bool admin_remove(const std::string& alias) { return core_.admin_remove(alias); }
    [[nodiscard]] std::vector<SharedFileEntry> admin_list_files() const {
        return core_.admin_list_files();
    }
    [[nodiscard]] std::vector<ClientSnapshot> admin_list_clients() const {
        return core_.admin_list_clients();
    }
    bool admin_kick(std::uint64_t id) { return core_.admin_kick(id); }
    [[nodiscard]] ServerStatus admin_status() const { return core_.admin_status(); }

    void submit_command(std::string line) { core_.submit_command(std::move(line)); }

    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return core_.bytes_sent(); }
    [[nodiscard]] std::uint64_t completed_downloads() const noexcept {
        return core_.completed_downloads();
    }
    [[nodiscard]] std::size_t client_count() const { return core_.client_count(); }
    [[nodiscard]] std::size_t downloads_in_progress() const {
        return core_.downloads_in_progress();
    }

private:
    void handle_client(net::Socket client, std::string peer);
    void finish_connection(const std::shared_ptr<ClientEntry>& entry) noexcept;
    void handle_list(net::Socket& client);
    void handle_download(net::Socket& client, const DownloadRequest& req, ClientEntry& info);

    ServerCore                core_;
    net::Socket               listener_;
    std::chrono::milliseconds drain_grace_;

    // Detached connection threads, drained to zero during teardown.
    std::atomic<int>        active_{0};
    std::mutex              drain_mutex_;
    std::condition_variable drain_cv_;
};

} // namespace fileshare
