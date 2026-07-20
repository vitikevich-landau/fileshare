#include "fileshare/v2/session.hpp"

#include <ctime>

#include "fileshare/net.hpp"

namespace fileshare::v2 {

// --- Session ----------------------------------------------------------------
void Session::authenticate(std::string login, Role role) {
    std::lock_guard<std::mutex> lk(mu_);
    login_ = std::move(login);
    role_ = role;
    authed_ = true;
}

Role Session::role() const {
    std::lock_guard<std::mutex> lk(mu_);
    return role_;
}

bool Session::authed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return authed_;
}

std::string Session::login() const {
    std::lock_guard<std::mutex> lk(mu_);
    return login_;
}

void Session::set_current_path(const std::string& p) {
    std::lock_guard<std::mutex> lk(mu_);
    current_path_ = p;
}

std::string Session::current_path() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_path_;
}

bool Session::send(const std::vector<std::uint8_t>& frame, bool blocking) {
    std::unique_lock<std::mutex> lk(send_mutex_, std::defer_lock);
    if (blocking) {
        lk.lock();
    } else if (!lk.try_lock()) {
        return false;   // busy (mid-transfer / responding) -- event bus skips
    }
    // Re-check dead_ UNDER the lock: a sender that was waiting for the mutex must
    // not write after close_send() marked the fd dead (and the owner closed it).
    if (dead_.load()) return false;
    try {
        net::send_all(handle_, frame.data(), frame.size());
        return true;
    } catch (const net::NetError&) {
        dead_.store(true);
        return false;
    }
}

void Session::close_send() {
    std::lock_guard<std::mutex> lk(send_mutex_);
    dead_.store(true);
}

void Session::touch() noexcept {
    last_activity_.store(static_cast<std::uint64_t>(std::time(nullptr)));
}

SessionSnapshot Session::snapshot() const {
    SessionSnapshot s;
    s.id = id_;
    s.ip = ip_;
    s.bytes_sent = bytes_sent_.load();
    {
        std::lock_guard<std::mutex> lk(mu_);
        s.login = login_;
        s.role = role_;
        s.current_path = current_path_;
    }
    return s;
}

// --- SessionRegistry --------------------------------------------------------
std::shared_ptr<Session> SessionRegistry::add(std::string ip, std::intptr_t handle) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::uint64_t id = next_id_++;
    auto s = std::make_shared<Session>(id, std::move(ip), handle);
    sessions_.emplace(id, s);
    return s;
}

void SessionRegistry::remove(std::uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.erase(id);
}

bool SessionRegistry::kick(std::uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return false;
    }
    net::shutdown_handle(it->second->handle());
    return true;
}

void SessionRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, s] : sessions_) {
        net::shutdown_handle(s->handle());
    }
}

void SessionRegistry::shutdown_idle() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, s] : sessions_) {
        if (s->current_path().empty()) {
            net::shutdown_handle(s->handle());
        }
    }
}

std::size_t SessionRegistry::broadcast(std::uint32_t sub_bit,
                                       const std::vector<std::uint8_t>& frame) {
    // Snapshot the shared_ptrs under the lock, then send outside it so a slow
    // send never blocks other connections from registering/leaving.
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, s] : sessions_) {
            if (s->subscription() & sub_bit) targets.push_back(s);
        }
    }
    std::size_t delivered = 0;
    for (auto& s : targets) {
        if (s->send(frame, /*blocking=*/false)) ++delivered;
    }
    return delivered;
}

std::size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}

std::size_t SessionRegistry::downloading_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t n = 0;
    for (const auto& [id, s] : sessions_) {
        if (!s->current_path().empty()) ++n;
    }
    return n;
}

std::size_t SessionRegistry::sessions_for_login(const std::string& login) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t n = 0;
    for (const auto& [id, s] : sessions_) {
        if (s->login() == login) ++n;
    }
    return n;
}

std::vector<SessionSnapshot> SessionRegistry::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<SessionSnapshot> out;
    out.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_) {
        out.push_back(s->snapshot());
    }
    return out;
}

} // namespace fileshare::v2
