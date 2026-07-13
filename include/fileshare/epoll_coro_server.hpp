#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fileshare/client_registry.hpp"
#include "fileshare/config.hpp"
#include "fileshare/server_core.hpp"

namespace fileshare {

// M6 stretch: the same epoll server, but the per-connection state machine is a
// C++20 coroutine -- the connection logic reads linearly (co_await a readable /
// writable socket) instead of the callback state machine of EpollServer. Single
// reactor thread: it runs epoll_wait and resumes the coroutine waiting on each
// ready fd, so connection state needs no locking. Shares ServerCore with the
// other servers. Linux-only.
class CoroServer {
public:
    explicit CoroServer(Catalog catalog, std::string config_path = {},
                        std::chrono::milliseconds drain_grace = std::chrono::seconds(5));
    ~CoroServer();
    CoroServer(const CoroServer&) = delete;
    CoroServer& operator=(const CoroServer&) = delete;

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

    // --- Coroutine plumbing (defined in the .cpp) ---------------------------
    struct Task;   // connection coroutine return type
    struct Conn;   // per-connection state

    // Called by an awaiter: register `fd` for `events` and remember the handle
    // to resume when it becomes ready.
    void arm(int fd, std::uint32_t events, std::coroutine_handle<> h);

private:
    Task handle_connection(int fd, std::shared_ptr<ClientEntry> entry);
    void accept_new();
    void resume_fd(int fd);
    void close_conn(int fd);
    void pump_events(int timeout_ms);

    ServerCore                core_;
    std::chrono::milliseconds drain_grace_;
    int                       epoll_fd_ = -1;
    int                       listener_fd_ = -1;
    std::unordered_map<int, std::unique_ptr<Conn>> conns_;
};

} // namespace fileshare
