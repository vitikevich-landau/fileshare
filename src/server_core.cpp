#include "fileshare/server_core.hpp"

#include <iostream>
#include <sstream>
#include <vector>

#include "fileshare/checksum.hpp"
#include "fileshare/protocol.hpp"

namespace fileshare {

std::vector<std::uint8_t> ServerCore::build_list_response() const {
    std::vector<ListEntry> entries;
    {
        std::lock_guard<std::mutex> lock(catalog_mutex_);
        for (const auto& e : catalog_.entries()) {
            ListEntry le;
            le.alias = e.alias;
            le.size = e.size_bytes;
            le.checksum = e.checksum;
            entries.push_back(std::move(le));
        }
    }
    return encode_list_response(entries);
}

std::optional<SharedFileEntry> ServerCore::resolve_download(const std::string& alias) const {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    return catalog_.find(alias);
}

// --- Admin command queue ----------------------------------------------------
void ServerCore::submit_command(std::string line) {
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    cmd_queue_.push_back(std::move(line));
}

void ServerCore::drain_commands() {
    for (;;) {
        std::string line;
        {
            std::lock_guard<std::mutex> lock(cmd_mutex_);
            if (cmd_queue_.empty()) {
                break;
            }
            line = std::move(cmd_queue_.front());
            cmd_queue_.pop_front();
        }
        execute_command(line);
    }
}

void ServerCore::execute_command(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd.empty()) {
        return;
    }

    // A misbehaving command must never escape the engine thread / teardown.
    try {
    if (cmd == "help") {
        std::cout << "commands: add <path> [alias], remove <alias>, list, clients, "
                     "status, kick <id>, shutdown, help\n";
    } else if (cmd == "add") {
        std::string path;
        std::string alias;
        in >> path >> alias;
        if (path.empty()) {
            std::cout << "usage: add <path> [alias]\n";
            return;
        }
        const auto outcome =
            admin_add(path, alias.empty() ? std::nullopt : std::optional<std::string>(alias));
        if (outcome.ok) {
            std::cout << "added: " << outcome.entry.alias << " (" << outcome.entry.size_bytes
                      << " bytes)\n";
        } else {
            std::cout << "add failed: " << outcome.error << "\n";
        }
    } else if (cmd == "remove") {
        std::string alias;
        in >> alias;
        std::cout << (admin_remove(alias) ? "removed\n" : "no such alias\n");
    } else if (cmd == "list") {
        const auto files = admin_list_files();
        std::cout << files.size() << " file(s):\n";
        for (const auto& e : files) {
            std::cout << "  " << e.alias << "  " << e.size_bytes << " bytes  "
                      << to_hex(e.checksum).substr(0, 8) << "\n";
        }
    } else if (cmd == "clients") {
        const auto clients = admin_list_clients();
        std::cout << clients.size() << " client(s):\n";
        for (const auto& c : clients) {
            std::cout << "  #" << c.id << "  " << c.peer << "  "
                      << (c.current_alias.empty() ? "(idle)" : c.current_alias) << "  "
                      << c.bytes_sent << " bytes\n";
        }
    } else if (cmd == "status") {
        const auto s = admin_status();
        std::cout << "uptime " << s.uptime_seconds << "s, sent " << s.bytes_sent << " bytes, "
                  << s.completed_downloads << " downloads, " << s.active_connections
                  << " active, " << s.shared_files << " files\n";
    } else if (cmd == "kick") {
        std::uint64_t id = 0;
        in >> id;
        std::cout << (admin_kick(id) ? "kicked\n" : "no such client\n");
    } else if (cmd == "shutdown") {
        std::cout << "shutting down...\n";
        request_stop();
    } else {
        std::cout << "unknown command: " << cmd << " (try 'help')\n";
    }
    } catch (const std::exception& e) {
        std::cerr << "command error: " << e.what() << "\n";
    }
}

// --- Admin surface ----------------------------------------------------------
AddOutcome ServerCore::admin_add(const std::string& path, const std::optional<std::string>& alias) {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    auto outcome = catalog_.add(path, alias);
    if (outcome.ok && !config_path_.empty()) {
        try {
            catalog_.save(config_path_);
        } catch (const ConfigError& e) {
            std::cerr << "warning: could not persist config: " << e.what() << "\n";
        }
    }
    return outcome;
}

bool ServerCore::admin_remove(const std::string& alias) {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    const bool removed = catalog_.remove(alias);
    if (removed && !config_path_.empty()) {
        try {
            catalog_.save(config_path_);
        } catch (const ConfigError& e) {
            std::cerr << "warning: could not persist config: " << e.what() << "\n";
        }
    }
    return removed;
}

std::vector<SharedFileEntry> ServerCore::admin_list_files() const {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    return catalog_.entries();
}

std::vector<ClientSnapshot> ServerCore::admin_list_clients() const {
    return registry_.snapshot();
}

bool ServerCore::admin_kick(std::uint64_t id) {
    return registry_.kick(id);
}

ServerStatus ServerCore::admin_status() const {
    ServerStatus status;
    const auto now = std::chrono::steady_clock::now();
    status.uptime_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
    status.bytes_sent = bytes_sent_.load();
    status.completed_downloads = completed_.load();
    status.active_connections = registry_.size();
    {
        std::lock_guard<std::mutex> lock(catalog_mutex_);
        status.shared_files = catalog_.size();
    }
    return status;
}

} // namespace fileshare
