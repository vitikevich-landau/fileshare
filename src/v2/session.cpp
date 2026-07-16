#include "fileshare/v2/session.hpp"

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
