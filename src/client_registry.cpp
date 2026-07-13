#include "fileshare/client_registry.hpp"

#include "fileshare/net.hpp"

namespace fileshare {

void ClientEntry::set_alias(const std::string& alias) {
    std::lock_guard<std::mutex> lock(alias_mutex_);
    current_alias_ = alias;
}

std::string ClientEntry::alias() const {
    std::lock_guard<std::mutex> lock(alias_mutex_);
    return current_alias_;
}

std::shared_ptr<ClientEntry> ClientRegistry::add(std::string peer, std::intptr_t handle) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::uint64_t id = next_id_++;
    auto entry = std::make_shared<ClientEntry>(id, std::move(peer), handle);
    clients_.emplace(id, entry);
    return entry;
}

void ClientRegistry::remove(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    clients_.erase(id);
}

bool ClientRegistry::kick(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = clients_.find(id);
    if (it == clients_.end()) {
        return false;
    }
    net::shutdown_handle(it->second->handle());
    return true;
}

void ClientRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& kv : clients_) {
        net::shutdown_handle(kv.second->handle());
    }
}

std::size_t ClientRegistry::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return clients_.size();
}

std::vector<ClientSnapshot> ClientRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ClientSnapshot> out;
    out.reserve(clients_.size());
    for (const auto& kv : clients_) {
        const auto& e = kv.second;
        out.push_back(ClientSnapshot{e->id(), e->peer(), e->alias(), e->bytes_sent()});
    }
    return out;
}

} // namespace fileshare
