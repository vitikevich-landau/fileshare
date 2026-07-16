#pragma once

// Per-connection session state that lives AFTER the handshake, plus a
// thread-safe registry of live sessions (for admin listing / kick / drain).
// Richer than v1's ClientRegistry: carries login, role and subscription state.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fileshare/v2/protocol.hpp"

namespace fileshare::v2 {

struct SessionSnapshot {
    std::uint64_t id = 0;
    std::string   login;
    std::string   ip;
    Role          role = Role::ANONYMOUS;
    std::string   current_path;   // empty when idle
    std::uint64_t bytes_sent = 0;
    std::uint64_t speed_bps = 0;
};

// One connection's mutable state. Counters are atomic; the current path and the
// identity fields (login/role, set once at auth) are guarded by a small mutex.
class Session {
public:
    Session(std::uint64_t id, std::string ip, std::intptr_t handle)
        : id_(id), ip_(std::move(ip)), handle_(handle),
          connected_at_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] std::intptr_t handle() const noexcept { return handle_; }
    [[nodiscard]] const std::string& ip() const noexcept { return ip_; }

    // Identity, assigned once on AUTH_OK.
    void authenticate(std::string login, Role role);
    [[nodiscard]] Role role() const;
    [[nodiscard]] bool authed() const;
    [[nodiscard]] std::string login() const;

    // Subscription mask (SUBSCRIBE).
    void set_subscription(std::uint32_t mask) noexcept { sub_mask_.store(mask); }
    [[nodiscard]] std::uint32_t subscription() const noexcept { return sub_mask_.load(); }

    // Transfer accounting.
    void add_bytes(std::uint64_t n) noexcept { bytes_sent_.fetch_add(n); }
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_.load(); }
    void set_current_path(const std::string& p);
    [[nodiscard]] std::string current_path() const;

    [[nodiscard]] SessionSnapshot snapshot() const;

private:
    std::uint64_t id_;
    std::string   ip_;
    std::intptr_t handle_;
    std::chrono::steady_clock::time_point connected_at_;

    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint32_t> sub_mask_{0};

    mutable std::mutex mu_;
    bool        authed_ = false;
    Role        role_ = Role::ANONYMOUS;
    std::string login_;
    std::string current_path_;
};

// Thread-safe map of live sessions.
class SessionRegistry {
public:
    std::shared_ptr<Session> add(std::string ip, std::intptr_t handle);
    void remove(std::uint64_t id);

    bool kick(std::uint64_t id);          // half-close; true if it existed
    void shutdown_all();                  // half-close every connection
    void shutdown_idle();                 // half-close only non-downloading ones

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t downloading_count() const;
    [[nodiscard]] std::size_t sessions_for_login(const std::string& login) const;
    [[nodiscard]] std::vector<SessionSnapshot> snapshot() const;

private:
    mutable std::mutex mu_;
    std::map<std::uint64_t, std::shared_ptr<Session>> sessions_;
    std::uint64_t next_id_ = 1;
};

} // namespace fileshare::v2
