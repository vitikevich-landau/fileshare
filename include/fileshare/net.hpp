#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "fileshare/protocol.hpp"

// Thin cross-platform TCP layer (Winsock on Windows, BSD sockets elsewhere).
// The public interface deliberately avoids including <winsock2.h>: a socket is
// stored as a std::intptr_t, so consumers don't inherit Windows header order
// constraints. -1 is the invalid sentinel on both platforms (Winsock's
// INVALID_SOCKET is (SOCKET)~0, i.e. -1 when viewed as intptr_t).
namespace fileshare::net {

inline constexpr std::intptr_t kInvalidSocket = -1;

class NetError : public std::runtime_error {
public:
    explicit NetError(const std::string& what) : std::runtime_error(what) {}
};

// Initialise networking: WSAStartup on Windows, ignore SIGPIPE on POSIX.
// Idempotent; called automatically by tcp_connect/tcp_listen.
void startup();

// RAII owner of a socket handle: closes on destruction, move-only.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(std::intptr_t handle) noexcept : handle_(handle) {}
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidSocket; }
    [[nodiscard]] std::intptr_t handle() const noexcept { return handle_; }
    void close() noexcept;

private:
    std::intptr_t handle_ = kInvalidSocket;
};

// Client: resolve host and connect. Throws NetError on failure.
[[nodiscard]] Socket tcp_connect(const std::string& host, std::uint16_t port);

// Server: create a listening socket bound to `port` (0 = OS-assigned). The
// actual bound port is written to *bound_port when non-null. Throws NetError.
[[nodiscard]] Socket tcp_listen(std::uint16_t port, std::uint16_t* bound_port = nullptr,
                                int backlog = 16);

// Accept one connection. Returns std::nullopt if the listener is non-blocking
// and no connection is ready, or a half-open connection was aborted between
// readiness and accept (so a serve loop can retry instead of blocking). The
// peer "ip:port" is written to *peer when non-null. The returned socket is
// always in blocking mode. Throws NetError on a real error.
[[nodiscard]] std::optional<Socket> tcp_accept(Socket& listener, std::string* peer = nullptr);

// Switch a socket between blocking and non-blocking mode. Throws NetError.
void set_nonblocking(Socket& s, bool on);

// Half-close a socket by raw handle, interrupting a peer that is blocked in
// recv/send on it (used by `kick`). No-op on an invalid handle; never throws.
void shutdown_handle(std::intptr_t handle) noexcept;

// Block up to timeout_ms for `s` to become readable. Returns true if readable,
// false on timeout. Throws NetError on a socket error. Lets an accept loop stay
// responsive to a stop flag without closing the socket across threads.
[[nodiscard]] bool wait_readable(Socket& s, int timeout_ms);

// Send exactly `len` bytes (short-write loop). Throws NetError.
void send_all(Socket& s, const std::uint8_t* data, std::size_t len);
inline void send_all(Socket& s, const std::vector<std::uint8_t>& buf) {
    send_all(s, buf.data(), buf.size());
}

// Read exactly `len` bytes. Returns the count actually read; a value < len
// means the peer closed the connection. Throws NetError on a socket error.
[[nodiscard]] std::size_t recv_exact(Socket& s, std::uint8_t* dst, std::size_t len);

// Send a framed message (5-byte header + payload) without concatenating into a
// single buffer -- used for the CHUNK_DATA stream to avoid copying file data.
void send_message(Socket& s, MessageType type, const std::uint8_t* payload, std::size_t len);

// Receive one complete frame. Returns std::nullopt on a clean connection close
// at a frame boundary. Throws ProtocolError on a malformed/oversize/truncated
// frame; NetError on a socket error.
[[nodiscard]] std::optional<Frame> recv_message(Socket& s);

} // namespace fileshare::net
