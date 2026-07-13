#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "fileshare/config.hpp"
#include "fileshare/net.hpp"
#include "fileshare/protocol.hpp"

namespace fileshare {

// M1 server: blocking, serves one client at a time. LIST + DOWNLOAD end-to-end.
// Concurrency (thread-per-connection) and the live admin console arrive in M2.
class Server {
public:
    explicit Server(Catalog catalog) : catalog_(std::move(catalog)) {}

    // Bind + listen; returns the actual bound port (useful with port 0).
    // Throws net::NetError on failure. Must be called before serve_forever().
    std::uint16_t listen(std::uint16_t port);

    // Accept and fully serve clients one at a time until stop() is signalled.
    // Blocking; typically run on its own thread in tests.
    void serve_forever();

    // Ask the accept loop to finish. Safe to call from another thread.
    void stop() noexcept { running_.store(false); }

    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_.load(); }
    [[nodiscard]] std::uint64_t completed_downloads() const noexcept { return completed_.load(); }

private:
    void handle_client(net::Socket& client, const std::string& peer);
    void handle_list(net::Socket& client);
    void handle_download(net::Socket& client, const DownloadRequest& req);

    Catalog                    catalog_;
    net::Socket                listener_;
    std::atomic<bool>          running_{false};
    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::mutex                 catalog_mutex_; // guards catalog_ once M2 adds live add/remove
};

} // namespace fileshare
