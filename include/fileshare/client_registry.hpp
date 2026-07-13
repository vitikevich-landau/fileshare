#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fileshare {

// Immutable snapshot of one connection for the `clients` admin command.
struct ClientSnapshot {
    std::uint64_t id = 0;
    std::string   peer;
    std::string   current_alias; // empty when idle
    std::uint64_t bytes_sent = 0;
};

// Per-connection record. Counters are atomic so the owning connection thread
// updates them without taking the registry lock; the alias is guarded on its
// own so `clients` can read "what is it downloading" mid-transfer.
class ClientEntry {
public:
    ClientEntry(std::uint64_t id, std::string peer, std::intptr_t handle)
        : id_(id), peer_(std::move(peer)), handle_(handle) {}

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] std::intptr_t handle() const noexcept { return handle_; }
    [[nodiscard]] const std::string& peer() const noexcept { return peer_; }

    void add_bytes(std::uint64_t n) noexcept { bytes_sent_.fetch_add(n); }
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_.load(); }

    void set_alias(const std::string& alias);
    [[nodiscard]] std::string alias() const;

private:
    std::uint64_t              id_;
    std::string                peer_;
    std::intptr_t              handle_;
    std::atomic<std::uint64_t> bytes_sent_{0};
    mutable std::mutex         alias_mutex_;
    std::string                current_alias_;
};

// Thread-safe map of active connections. add()/remove() are called by
// connection threads; kick()/shutdown_all()/snapshot()/size() by the admin
// engine. `kick` half-closes the socket under the lock, and connection threads
// remove() themselves before their socket closes, so a kicked/removed handle is
// never confused with a reused fd.
class ClientRegistry {
public:
    std::shared_ptr<ClientEntry> add(std::string peer, std::intptr_t handle);
    void remove(std::uint64_t id);
    bool kick(std::uint64_t id);   // half-close the connection; true if it existed
    void shutdown_all();           // half-close every connection (server teardown)

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::vector<ClientSnapshot> snapshot() const;

private:
    mutable std::mutex mu_;
    std::map<std::uint64_t, std::shared_ptr<ClientEntry>> clients_;
    std::uint64_t next_id_ = 1;
};

} // namespace fileshare
