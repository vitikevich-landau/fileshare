#include "fileshare/v2/auth.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "fileshare/v2/crypto.hpp"

using nlohmann::json;

namespace fileshare::v2 {

namespace {

const std::string SALT_PREFIX = "fileshare-v2:";
const std::string CLIENT_KEY_CONST = "Client Key";

std::vector<std::uint8_t> salt_for(const std::string& login) {
    const std::string s = SALT_PREFIX + login;
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// SaltedPassword and ClientKey (shared by client and server derivations).
crypto::Digest client_key_of(const std::string& password, const std::string& login,
                             std::uint32_t iters) {
    const std::vector<std::uint8_t> salted =
        crypto::pbkdf2_hmac_sha256(password, salt_for(login), iters, 32);
    const auto* ck = reinterpret_cast<const std::uint8_t*>(CLIENT_KEY_CONST.data());
    return crypto::hmac_sha256(salted.data(), salted.size(), ck, CLIENT_KEY_CONST.size());
}

std::vector<std::uint8_t> auth_message(const Challenge& challenge, const std::string& login) {
    std::vector<std::uint8_t> m(challenge.begin(), challenge.end());
    m.insert(m.end(), login.begin(), login.end());
    return m;
}

} // namespace

std::string role_to_string(Role r) {
    switch (r) {
        case Role::ADMIN: return "admin";
        case Role::USER:  return "user";
        case Role::ANONYMOUS: return "anonymous";
    }
    return "user";
}

std::optional<Role> role_from_string(const std::string& s) {
    if (s == "admin") return Role::ADMIN;
    if (s == "user")  return Role::USER;
    return std::nullopt;
}

User make_user(const std::string& login, Role role, const std::string& password,
               std::uint32_t iters) {
    const crypto::Digest ck = client_key_of(password, login, iters);
    const crypto::Digest sk = crypto::sha256(ck.data(), ck.size());
    User u;
    u.login = login;
    u.role = role;
    u.enabled = true;
    std::copy(sk.begin(), sk.end(), u.stored_key.begin());
    return u;
}

Proof compute_client_proof(const std::string& password, const std::string& login,
                           const Challenge& challenge, std::uint32_t iters) {
    const crypto::Digest client_key = client_key_of(password, login, iters);
    const crypto::Digest stored_key = crypto::sha256(client_key.data(), client_key.size());
    const std::vector<std::uint8_t> msg = auth_message(challenge, login);
    const crypto::Digest sig = crypto::hmac_sha256(stored_key.data(), stored_key.size(),
                                                   msg.data(), msg.size());
    Proof proof{};
    for (std::size_t i = 0; i < proof.size(); ++i) {
        proof[i] = static_cast<std::uint8_t>(client_key[i] ^ sig[i]);
    }
    return proof;
}

bool verify_client_proof(const User& user, const Challenge& challenge, const Proof& proof,
                         std::uint32_t /*iters*/) {
    // Verification does not re-run the KDF: it uses the stored StoredKey plus the
    // proof, so `iters` is irrelevant here (kept for signature symmetry).
    if (!user.enabled) {
        return false;
    }
    const std::vector<std::uint8_t> msg = auth_message(challenge, user.login);
    const crypto::Digest sig = crypto::hmac_sha256(user.stored_key.data(), user.stored_key.size(),
                                                   msg.data(), msg.size());
    // recovered ClientKey = proof XOR sig
    std::array<std::uint8_t, 32> recovered{};
    for (std::size_t i = 0; i < recovered.size(); ++i) {
        recovered[i] = static_cast<std::uint8_t>(proof[i] ^ sig[i]);
    }
    const crypto::Digest check = crypto::sha256(recovered.data(), recovered.size());
    return crypto::constant_time_equal(check.data(), user.stored_key.data(), user.stored_key.size());
}

// --- UserDb -----------------------------------------------------------------
UserDb UserDb::load(const std::string& path) {
    UserDb db;
    std::ifstream in(path);
    if (!in) {
        return db;   // missing file -> empty (no-auth bootstrap)
    }
    json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return db;   // corrupt file -> treat as empty; caller/logs can flag it
    }
    if (!j.is_object() || !j.contains("users") || !j["users"].is_array()) {
        return db;
    }
    for (const auto& uj : j["users"]) {
        try {
            User u;
            u.login = uj.at("login").get<std::string>();
            const auto role = role_from_string(uj.value("role", "user"));
            u.role = role.value_or(Role::USER);
            u.enabled = uj.value("enabled", true);
            const std::string hex = uj.at("stored_key").get<std::string>();
            const auto raw = crypto::from_hex(hex);
            if (raw.size() != u.stored_key.size()) continue;
            std::copy(raw.begin(), raw.end(), u.stored_key.begin());
            db.set(u);
        } catch (const std::exception&) {
            // skip malformed record, keep the rest
        }
    }
    return db;
}

void UserDb::save(const std::string& path) const {
    json arr = json::array();
    for (const auto& u : users_) {
        arr.push_back({
            {"login", u.login},
            {"role", role_to_string(u.role)},
            {"stored_key", crypto::to_hex(u.stored_key.data(), u.stored_key.size())},
            {"enabled", u.enabled},
        });
    }
    json j = {{"users", arr}};
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot write users file: " + path);
    }
    out << j.dump(2) << "\n";
    if (!out) {
        throw std::runtime_error("write failed: " + path);
    }
}

std::optional<User> UserDb::find(const std::string& login) const {
    for (const auto& u : users_) {
        if (u.login == login) return u;
    }
    return std::nullopt;
}

void UserDb::set(const User& u) {
    for (auto& existing : users_) {
        if (existing.login == u.login) {
            existing = u;
            return;
        }
    }
    users_.push_back(u);
}

bool UserDb::remove(const std::string& login) {
    for (auto it = users_.begin(); it != users_.end(); ++it) {
        if (it->login == login) {
            users_.erase(it);
            return true;
        }
    }
    return false;
}

// --- AuthGuard --------------------------------------------------------------
bool AuthGuard::banned(const std::string& ip, Clock::time_point now) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = ips_.find(ip);
    return it != ips_.end() && now < it->second.ban_until;
}

void AuthGuard::fail(const std::string& ip, Clock::time_point now,
                     std::uint32_t ban_seconds, int threshold) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = ips_[ip];
    ++e.failures;
    if (e.failures >= threshold) {
        e.ban_until = now + std::chrono::seconds(ban_seconds);
        e.failures = 0;   // start a fresh window after the ban is set
    }
}

void AuthGuard::succeed(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu_);
    ips_.erase(ip);
}

} // namespace fileshare::v2
