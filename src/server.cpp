#include "fileshare/server.hpp"

#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

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
    core_.mark_started();
    return bound;
}

void Server::serve_forever() {
    // The teardown MUST run even on exception: detached connection threads
    // dereference `this`/core_, so they must all finish before we return.
    std::exception_ptr pending;
    try {
        while (core_.accepting()) {
            core_.drain_commands();

            std::optional<net::Socket> client;
            std::string peer;
            try {
                if (!net::wait_readable(listener_, 200)) {
                    continue;
                }
                client = net::tcp_accept(listener_, &peer);
            } catch (const net::NetError&) {
                if (!core_.accepting()) {
                    break;
                }
                continue;
            }
            if (!client) {
                continue;
            }

            active_.fetch_add(1);
            try {
                std::thread(&Server::handle_client, this, std::move(*client), std::move(peer))
                    .detach();
            } catch (...) {
                // std::system_error (OS can't start the thread) OR std::bad_alloc
                // (thread state allocation) -- no thread ran either way, so undo
                // the active_ bump and drop just this connection, otherwise the
                // final drain wait would block forever on a phantom count.
                active_.fetch_sub(1);
            }
        }
    } catch (...) {
        pending = std::current_exception();
    }

    // Graceful drain: close idle connections now, then give active downloads up
    // to drain_grace_ to finish before force-closing any stragglers (§3.1).
    core_.registry().shutdown_idle();
    {
        std::unique_lock<std::mutex> lock(drain_mutex_);
        drain_cv_.wait_for(lock, drain_grace_, [this] { return active_.load() == 0; });
    }
    core_.registry().shutdown_all();
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
        entry = core_.registry().add(std::move(peer), client.handle());
        bool serving = true;
        while (serving && core_.accepting()) {
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
                    serving = false;
                    break;
            }
        }
    } catch (const ProtocolError&) {
        // Malformed frame/payload: drop this connection only (§7).
    } catch (const net::NetError&) {
        // Peer reset / kicked / socket error: drop this connection only.
    } catch (...) {
        // Anything else (e.g. std::bad_alloc): drop this connection, not the server.
    }

    finish_connection(entry);
}

void Server::finish_connection(const std::shared_ptr<ClientEntry>& entry) noexcept {
    if (entry) {
        core_.registry().remove(entry->id()); // deregister before the socket closes
    }
    std::lock_guard<std::mutex> lock(drain_mutex_);
    active_.fetch_sub(1);
    // notify_all() under the lock: otherwise the waiter in serve_forever() can
    // observe active_ == 0 and destroy this condition_variable before we notify.
    drain_cv_.notify_all();
}

void Server::handle_list(net::Socket& client) {
    net::send_all(client, core_.build_list_response());
}

void Server::handle_download(net::Socket& client, const DownloadRequest& req, ClientEntry& info) {
    // Copy the catalog entry (under the lock, inside resolve_download) then
    // release it: the transfer runs without holding up admin add/remove, and an
    // already-open file keeps streaming even if the alias is removed (§7).
    const std::optional<SharedFileEntry> entry = core_.resolve_download(req.alias);
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
    // Stream to completion (no accepting() check): an in-flight download runs to
    // the end during a graceful drain; only a force-close after the grace period
    // (send throws NetError) aborts it.
    for (;;) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            net::send_message(client, MessageType::CHUNK_DATA, buf.data(),
                              static_cast<std::size_t>(got));
            core_.add_bytes(static_cast<std::uint64_t>(got));
            info.add_bytes(static_cast<std::uint64_t>(got));
        }
        if (!in) {
            break;
        }
    }

    net::send_all(client, encode_download_done(entry->checksum));
    core_.inc_completed();
    info.set_alias("");
}

} // namespace fileshare
