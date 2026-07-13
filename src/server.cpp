#include "fileshare/server.hpp"

#include <fstream>
#include <optional>
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
    // Non-blocking so accept() never blocks after wait_readable(): a peer that
    // resets between readiness and accept yields nullopt instead of wedging.
    net::set_nonblocking(listener_, true);
    running_.store(true);
    return bound;
}

void Server::serve_forever() {
    while (running_.load()) {
        std::optional<net::Socket> client;
        std::string peer;
        try {
            // Poll with a timeout so stop() is observed without closing the
            // listening socket from another thread. Both the poll and the
            // accept are guarded so a stray socket error drops at most one
            // connection instead of escaping the thread (std::terminate).
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
            continue; // spurious readiness or an aborted half-open connection
        }
        handle_client(*client, peer);
    }
}

void Server::handle_client(net::Socket& client, const std::string& peer) {
    (void)peer; // logged in M2's admin console
    try {
        while (running_.load()) {
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
                    handle_download(client, req);
                    break;
                }
                case MessageType::PING:
                    net::send_all(client, encode_pong());
                    break;
                default:
                    send_error(client, ErrorCode::BAD_REQUEST, "unexpected message type");
                    return; // drop connection on protocol misuse
            }
        }
    } catch (const ProtocolError&) {
        // Malformed frame/payload: drop this connection only (§7).
    } catch (const net::NetError&) {
        // Peer reset or socket error: drop this connection only.
    }
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

void Server::handle_download(net::Socket& client, const DownloadRequest& req) {
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

    std::vector<std::uint8_t> buf(CHUNK_SIZE);
    while (running_.load()) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            net::send_message(client, MessageType::CHUNK_DATA, buf.data(),
                              static_cast<std::size_t>(got));
            bytes_sent_.fetch_add(static_cast<std::uint64_t>(got));
        }
        if (!in) {
            break; // EOF (or read error; the DONE checksum lets the client detect corruption)
        }
    }

    net::send_all(client, encode_download_done(entry->checksum));
    completed_.fetch_add(1);
}

} // namespace fileshare
