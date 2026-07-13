#include "fileshare/server.hpp"

#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "fileshare/checksum.hpp"

namespace fileshare {
namespace {

void send_error(net::Socket& client, ErrorCode code, const std::string& message) {
    ErrorMessage err;
    err.code = code;
    err.message = message;
    net::send_all(client, encode_error(err));
}

} // namespace

std::uint16_t Server::listen(std::uint16_t port) {
    std::uint16_t bound = 0;
    listener_ = net::tcp_listen(port, &bound);
    net::set_nonblocking(listener_, true);
    start_time_ = std::chrono::steady_clock::now();
    running_.store(true);
    return bound;
}

void Server::serve_forever() {
    // Whatever happens in the accept loop, the teardown below MUST run: detached
    // connection threads dereference `this`, so they must all finish before we
    // return (server.hpp contract). Any stray exception is deferred past
    // teardown and rethrown, so the caller still learns of it without a UAF.
    std::exception_ptr pending;
    try {
        while (running_.load()) {
            drain_commands();

            std::optional<net::Socket> client;
            std::string peer;
            try {
                if (!net::wait_readable(listener_, 200)) {
                    continue;
                }
                client = net::tcp_accept(listener_, &peer);
            } catch (const net::NetError&) {
                if (!running_.load()) {
                    break;
                }
                continue;
            }
            if (!client) {
                continue;
            }

            // One detached thread per connection. active_ is bumped in the
            // parent before the thread starts so teardown's drain never races a
            // spawn; handle_client always decrements it (finish_connection).
            active_.fetch_add(1);
            try {
                std::thread(&Server::handle_client, this, std::move(*client), std::move(peer))
                    .detach();
            } catch (const std::system_error&) {
                active_.fetch_sub(1); // could not start a thread; drop this connection
            }
        }
    } catch (...) {
        pending = std::current_exception();
    }

    // Teardown: break every live connection so its thread unblocks, then wait
    // for all connection threads to finish before returning.
    registry_.shutdown_all();
    {
        std::unique_lock<std::mutex> lock(drain_mutex_);
        drain_cv_.wait(lock, [this] { return active_.load() == 0; });
    }

    if (pending) {
        std::rethrow_exception(pending);
    }
}

void Server::handle_client(net::Socket client, std::string peer) {
    std::shared_ptr<ClientEntry> entry;
    try {
        entry = registry_.add(std::move(peer), client.handle());
        bool serving = true;
        while (serving && running_.load()) {
            const std::optional<Frame> frame = net::recv_message(client);
            if (!frame) {
                break; // clean close
            }
            switch (frame->type) {
                case MessageType::LIST_REQUEST:
                    handle_list(client);
                    break;
                case MessageType::DOWNLOAD_REQUEST: {
                    const DownloadRequest req =
                        parse_download_request(frame->payload.data(), frame->payload.size());
                    handle_download(client, req, *entry);
                    break;
                }
                case MessageType::PING:
                    net::send_all(client, encode_pong());
                    break;
                default:
                    send_error(client, ErrorCode::BAD_REQUEST, "unexpected message type");
                    serving = false; // drop connection on protocol misuse
                    break;
            }
        }
    } catch (const ProtocolError&) {
        // Malformed frame/payload: drop this connection only (§7).
    } catch (const net::NetError&) {
        // Peer reset / kicked / socket error: drop this connection only.
    } catch (...) {
        // Anything else (e.g. std::bad_alloc): still drop just this connection
        // rather than std::terminate the whole server from a detached thread.
    }

    // Runs on every path (including registry_.add throwing above), so active_
    // is always decremented and teardown never waits forever. The Socket is
    // closed as `client` goes out of scope, i.e. after we deregister.
    finish_connection(entry);
}

void Server::finish_connection(const std::shared_ptr<ClientEntry>& entry) noexcept {
    if (entry) {
        registry_.remove(entry->id()); // deregister before the socket closes
    }
    std::lock_guard<std::mutex> lock(drain_mutex_);
    active_.fetch_sub(1);
    // notify_all() must be inside the lock: otherwise the waiter in
    // serve_forever() can observe active_ == 0, return, and destroy the Server
    // (and this condition_variable) before notify_all() runs.
    drain_cv_.notify_all();
}

void Server::handle_list(net::Socket& client) {
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
    net::send_all(client, encode_list_response(entries));
}

void Server::handle_download(net::Socket& client, const DownloadRequest& req, ClientEntry& info) {
    // Copy the catalog entry under the lock, then release it: the (possibly
    // long) transfer runs without holding up admin add/remove, and an already
    // opened file keeps streaming even if the alias is removed mid-download.
    std::optional<SharedFileEntry> entry;
    {
        std::lock_guard<std::mutex> lock(catalog_mutex_);
        entry = catalog_.find(req.alias);
    }
    if (!entry) {
        send_error(client, ErrorCode::FILE_NOT_FOUND, "no such alias: " + req.alias);
        return;
    }
    if (req.offset > entry->size_bytes) {
        send_error(client, ErrorCode::UNSUPPORTED_OFFSET, "offset beyond end of file");
        return;
    }

    std::ifstream in(entry->path, std::ios::binary);
    if (!in) {
        send_error(client, ErrorCode::INTERNAL_ERROR, "cannot open file on server");
        return;
    }
    if (req.offset > 0) {
        in.seekg(static_cast<std::streamoff>(req.offset));
    }

    info.set_alias(req.alias);
    std::vector<std::uint8_t> buf(CHUNK_SIZE);
    while (running_.load()) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            net::send_message(client, MessageType::CHUNK_DATA, buf.data(),
                              static_cast<std::size_t>(got));
            const auto n = static_cast<std::uint64_t>(got);
            bytes_sent_.fetch_add(n);
            info.add_bytes(n);
        }
        if (!in) {
            break;
        }
    }

    net::send_all(client, encode_download_done(entry->checksum));
    completed_.fetch_add(1);
    info.set_alias("");
}

// --- Admin command queue ----------------------------------------------------
void Server::submit_command(std::string line) {
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    cmd_queue_.push_back(std::move(line));
}

void Server::drain_commands() {
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

void Server::execute_command(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd.empty()) {
        return;
    }

    // A misbehaving command (e.g. `add` with a non-UTF-8 alias making save()
    // fail, or an allocation failure) must never escape the engine thread and
    // skip serve_forever()'s teardown.
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
        stop();
    } else {
        std::cout << "unknown command: " << cmd << " (try 'help')\n";
    }
    } catch (const std::exception& e) {
        std::cerr << "command error: " << e.what() << "\n";
    }
}

// --- Admin surface ----------------------------------------------------------
AddOutcome Server::admin_add(const std::string& path, const std::optional<std::string>& alias) {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    auto outcome = catalog_.add(path, alias);
    if (outcome.ok && !config_path_.empty()) {
        try {
            catalog_.save(config_path_);
        } catch (const ConfigError& e) {
            // Keep the in-memory addition; report the persistence failure.
            std::cerr << "warning: could not persist config: " << e.what() << "\n";
        }
    }
    return outcome;
}

bool Server::admin_remove(const std::string& alias) {
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

std::vector<SharedFileEntry> Server::admin_list_files() const {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    return catalog_.entries();
}

std::vector<ClientSnapshot> Server::admin_list_clients() const {
    return registry_.snapshot();
}

bool Server::admin_kick(std::uint64_t id) {
    return registry_.kick(id);
}

ServerStatus Server::admin_status() const {
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
