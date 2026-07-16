#pragma once

// Authentication: user database + a SCRAM-like challenge-response so the
// password never crosses the wire and a stolen users.json cannot be replayed to
// log in (server stores only StoredKey = SHA256(ClientKey)).
//
// Derivation (salt is deterministic from the login, so the client needs no
// extra round trip -- the handshake stays HELLO -> HELLO_OK -> AUTH_REQUEST):
//   SaltedPassword = PBKDF2-HMAC-SHA256(password, "fileshare-v2:"+login, ITERS)
//   ClientKey      = HMAC(SaltedPassword, "Client Key")
//   StoredKey      = SHA256(ClientKey)                  <- persisted server-side
//   AuthMessage    = challenge(16) || login
//   ClientProof    = ClientKey XOR HMAC(StoredKey, AuthMessage)   <- on the wire
// Server recovers ClientKey = ClientProof XOR HMAC(StoredKey, AuthMessage) and
// checks SHA256(ClientKey) == StoredKey.

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fileshare/v2/protocol.hpp"

namespace fileshare::v2 {

// Protocol-wide PBKDF2 iteration count. Fixed so client and server agree without
// negotiation; bumping it requires re-creating users (documented tradeoff).
inline constexpr std::uint32_t AUTH_PBKDF2_ITERS = 200000;

using StoredKey = std::array<std::uint8_t, 32>;

struct User {
    std::string login;
    Role        role = Role::USER;
    StoredKey   stored_key{};
    bool        enabled = true;
};

// Build a user record from a plaintext password (server-side, e.g. --add-user).
// `iters` must match the value the server advertises in HELLO_OK.
[[nodiscard]] User make_user(const std::string& login, Role role, const std::string& password,
                             std::uint32_t iters = AUTH_PBKDF2_ITERS);

// Client side: compute the ClientProof to put in AUTH_REQUEST. `iters` comes
// from HELLO_OK.pbkdf2_iters.
[[nodiscard]] Proof compute_client_proof(const std::string& password, const std::string& login,
                                         const Challenge& challenge,
                                         std::uint32_t iters = AUTH_PBKDF2_ITERS);

// Server side: verify a ClientProof against a stored user. Constant-time.
[[nodiscard]] bool verify_client_proof(const User& user, const Challenge& challenge,
                                       const Proof& proof, std::uint32_t iters = AUTH_PBKDF2_ITERS);

[[nodiscard]] std::string role_to_string(Role r);
[[nodiscard]] std::optional<Role> role_from_string(const std::string& s);

// User database persisted as JSON (users.json). Missing file -> empty db, which
// puts the server in no-auth bootstrap mode (see server handshake).
class UserDb {
public:
    [[nodiscard]] static UserDb load(const std::string& path);   // never throws on missing file
    void save(const std::string& path) const;                    // throws on write failure

    [[nodiscard]] bool empty() const noexcept { return users_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return users_.size(); }
    [[nodiscard]] std::optional<User> find(const std::string& login) const;

    void set(const User& u);              // add or replace by login
    bool remove(const std::string& login);
    [[nodiscard]] const std::vector<User>& users() const noexcept { return users_; }

private:
    std::vector<User> users_;
};

// Per-IP brute-force throttle: after `threshold` failed auths an IP is banned
// for `ban_seconds`. Time is passed in so it can be unit-tested deterministically.
class AuthGuard {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] bool banned(const std::string& ip, Clock::time_point now) const;
    void fail(const std::string& ip, Clock::time_point now,
              std::uint32_t ban_seconds, int threshold = 3);
    void succeed(const std::string& ip);

private:
    struct Entry { int failures = 0; Clock::time_point ban_until{}; };
    mutable std::mutex               mu_;
    std::map<std::string, Entry>     ips_;
};

} // namespace fileshare::v2
