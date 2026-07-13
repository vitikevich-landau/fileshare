#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fileshare/client_registry.hpp"
#include "fileshare/config.hpp"
#include "fileshare/protocol.hpp"
#include "fileshare/server_core.hpp"
#include "fileshare/thread_pool.hpp"

namespace fileshare {

struct EpollConn; // defined in epoll_server.cpp

// M4 server: a single epoll reactor thread + a worker thread pool (the ATM task
// queue). The reactor runs epoll_wait and submits each ready connection to the
// pool; EPOLLONESHOT guarantees exactly one worker touches a connection at a
// time, so per-connection state needs no locking. Sockets are non-blocking with
// a per-connection read/write state machine (partial frames, streamed downloads
// with EPOLLOUT backpressure). Linux-only; shares ServerCore with the portable
// M2 Server, so the catalog / registry / admin logic is not duplicated.
class EpollServer {
public:
    explicit EpollServer(Catalog catalog, std::string config_path = {}, std::size_t workers = 0,
                         std::chrono::milliseconds drain_grace = std::chrono::seconds(5));
    ~EpollServer();
    EpollServer(const EpollServer&) = delete;
    EpollServer& operator=(const EpollServer&) = delete;

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
    void accept_new();                                              // reactor thread
    void process(std::shared_ptr<EpollConn> conn, std::uint32_t events); // worker thread
    bool do_read(EpollConn& conn);
    void dispatch_available(EpollConn& conn);
    bool do_write(EpollConn& conn);
    void start_download(EpollConn& conn, const DownloadRequest& req);
    void refill_download(EpollConn& conn);
    bool rearm(EpollConn& conn);
    void close_conn(const std::shared_ptr<EpollConn>& conn);
    std::shared_ptr<EpollConn> lookup(int fd);

    ServerCore                core_;
    std::size_t               worker_count_;
    std::chrono::milliseconds drain_grace_;
    int                       epoll_fd_ = -1;
    int                       listener_fd_ = -1;

    std::mutex                                          conns_mutex_;
    std::unordered_map<int, std::shared_ptr<EpollConn>> conns_;
    std::unique_ptr<ThreadPool>                         pool_;
};

} // namespace fileshare
