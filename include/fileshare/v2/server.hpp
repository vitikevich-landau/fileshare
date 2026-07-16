#pragma once

// Thread-per-connection v2 server. Simple, cross-platform, and enough to carry
// the whole v2 feature set (sessions, auth, tree browsing, streaming, events,
// admin) through M7-M11. An epoll port can come later behind the same
// ServerContext, exactly as v1 grew from thread-per-connection to epoll.

#include <cstdint>

#include "fileshare/net.hpp"
#include "fileshare/v2/server_context.hpp"

namespace fileshare::v2 {

class Server {
public:
    explicit Server(ServerContext& ctx) : ctx_(ctx) {}

    // Bind a listening socket; returns the actually-bound port (useful with 0).
    std::uint16_t bind(std::uint16_t port);

    // Accept loop until stop() (or an ADMIN_SHUTDOWN / signal) flips the context
    // flag, then drain: let active downloads finish (up to grace_seconds), close
    // the rest, and persist the checksum cache. Must be called after bind().
    void serve(std::uint32_t grace_seconds = 30);

    // Request a graceful stop from another thread / a signal handler.
    void stop() noexcept { ctx_.request_stop(); }

private:
    ServerContext& ctx_;
    net::Socket    listener_;
};

// Handle one fully-accepted connection: register a session, run the handshake,
// then the request loop. Exposed for integration tests that drive it directly.
void handle_connection(net::Socket sock, ServerContext& ctx, std::string peer);

} // namespace fileshare::v2
