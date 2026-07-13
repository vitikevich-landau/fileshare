// Platform sockets must be included before anything that may pull <windows.h>.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <cerrno>
#  include <csignal>
#  include <cstring>
#endif

#include "fileshare/net.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <string>

namespace fileshare::net {
namespace {

#ifdef _WIN32
using native_socket = SOCKET;
constexpr native_socket kNativeInvalid = INVALID_SOCKET;
int         last_error() { return ::WSAGetLastError(); }
bool        is_eintr(int e) { return e == WSAEINTR; }
bool        is_wouldblock(int e) { return e == WSAEWOULDBLOCK; }
bool        is_conn_aborted(int e) { return e == WSAECONNRESET || e == WSAECONNABORTED; }
void        close_native(native_socket s) { ::closesocket(s); }
std::string error_string(int e) { return "winsock error " + std::to_string(e); }
using send_len_t = int;
using sockopt_ptr = const char*;
#else
using native_socket = int;
constexpr native_socket kNativeInvalid = -1;
int         last_error() { return errno; }
bool        is_eintr(int e) { return e == EINTR; }
bool        is_wouldblock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool        is_conn_aborted(int e) { return e == ECONNABORTED || e == ECONNRESET || e == EPROTO; }
void        close_native(native_socket s) { ::close(s); }
std::string error_string(int e) { return std::strerror(e); }
using send_len_t = std::size_t;
using sockopt_ptr = const void*;
#endif

native_socket native(const Socket& s) {
    return static_cast<native_socket>(s.handle());
}

// Largest single send/recv chunk. Bounds the value passed to Winsock's int-typed
// length parameter and keeps each syscall a sane size.
constexpr std::size_t kIoChunk = 1u << 20; // 1 MiB

} // namespace

void startup() {
#ifdef _WIN32
    static const bool initialized = [] {
        WSADATA data;
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            throw NetError("WSAStartup failed: " + std::to_string(rc));
        }
        std::atexit([] { ::WSACleanup(); });
        return true;
    }();
    (void)initialized;
#else
    static const bool ignored = [] {
        std::signal(SIGPIPE, SIG_IGN); // never die because a peer vanished mid-write
        return true;
    }();
    (void)ignored;
#endif
}

// --- Socket lifetime --------------------------------------------------------
Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidSocket;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalidSocket;
    }
    return *this;
}

void Socket::close() noexcept {
    if (handle_ != kInvalidSocket) {
        close_native(static_cast<native_socket>(handle_));
        handle_ = kInvalidSocket;
    }
}

// --- Connect / listen / accept ---------------------------------------------
Socket tcp_connect(const std::string& host, std::uint16_t port) {
    startup();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        throw NetError("getaddrinfo(" + host + ") failed");
    }

    native_socket fd = kNativeInvalid;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == kNativeInvalid) {
            continue;
        }
        if (::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) == 0) {
            break;
        }
        close_native(fd);
        fd = kNativeInvalid;
    }
    ::freeaddrinfo(res);

    if (fd == kNativeInvalid) {
        throw NetError("cannot connect to " + host + ":" + std::to_string(port));
    }
    return Socket(static_cast<std::intptr_t>(fd));
}

Socket tcp_listen(std::uint16_t port, std::uint16_t* bound_port, int backlog) {
    startup();

    const native_socket fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kNativeInvalid) {
        throw NetError("socket() failed: " + error_string(last_error()));
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<sockopt_ptr>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int e = last_error();
        close_native(fd);
        throw NetError("bind failed: " + error_string(e));
    }
    if (::listen(fd, backlog) != 0) {
        const int e = last_error();
        close_native(fd);
        throw NetError("listen failed: " + error_string(e));
    }
    if (bound_port != nullptr) {
        sockaddr_in got{};
        socklen_t got_len = sizeof(got);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &got_len) == 0) {
            *bound_port = ntohs(got.sin_port);
        }
    }
    return Socket(static_cast<std::intptr_t>(fd));
}

void set_nonblocking(Socket& s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1u : 0u;
    if (::ioctlsocket(native(s), FIONBIO, &mode) != 0) {
        throw NetError("ioctlsocket(FIONBIO) failed: " + error_string(last_error()));
    }
#else
    const int flags = ::fcntl(native(s), F_GETFL, 0);
    if (flags < 0) {
        throw NetError("fcntl(F_GETFL) failed: " + error_string(last_error()));
    }
    const int updated = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(native(s), F_SETFL, updated) != 0) {
        throw NetError("fcntl(F_SETFL) failed: " + error_string(last_error()));
    }
#endif
}

std::optional<Socket> tcp_accept(Socket& listener, std::string* peer) {
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    for (;;) {
        const native_socket fd =
            ::accept(native(listener), reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (fd == kNativeInvalid) {
            const int e = last_error();
            if (is_eintr(e)) {
                continue;
            }
            // Non-blocking listener with nothing ready, or a half-open peer that
            // reset between select() readiness and accept(): not an error, just
            // retry. This is the fix for the select-then-blocking-accept race.
            if (is_wouldblock(e) || is_conn_aborted(e)) {
                return std::nullopt;
            }
            throw NetError("accept failed: " + error_string(e));
        }

        Socket client(static_cast<std::intptr_t>(fd));
        // accept() may hand back a non-blocking socket (inherited on some
        // platforms); force blocking so the per-connection I/O is simple.
        set_nonblocking(client, false);
        if (peer != nullptr) {
            char ip[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            *peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
        }
        return client;
    }
}

bool wait_readable(Socket& s, int timeout_ms) {
    const native_socket fd = native(s);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    const int n = ::select(0, &rd, nullptr, nullptr, &tv);
#else
    const int n = ::select(fd + 1, &rd, nullptr, nullptr, &tv);
#endif
    if (n < 0) {
        const int e = last_error();
        if (is_eintr(e)) {
            return false;
        }
        throw NetError("select failed: " + error_string(e));
    }
    return n > 0;
}

// --- Byte I/O ---------------------------------------------------------------
void send_all(Socket& s, const std::uint8_t* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const std::size_t want = std::min(len - sent, kIoChunk);
        const auto n = ::send(native(s), reinterpret_cast<const char*>(data + sent),
                              static_cast<send_len_t>(want), 0);
        if (n <= 0) {
            const int e = last_error();
            if (n < 0 && is_eintr(e)) {
                continue;
            }
            throw NetError("send failed: " + error_string(e));
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::size_t recv_exact(Socket& s, std::uint8_t* dst, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
        const std::size_t want = std::min(len - got, kIoChunk);
        const auto n = ::recv(native(s), reinterpret_cast<char*>(dst + got),
                              static_cast<send_len_t>(want), 0);
        if (n == 0) {
            break; // peer closed
        }
        if (n < 0) {
            const int e = last_error();
            if (is_eintr(e)) {
                continue;
            }
            throw NetError("recv failed: " + error_string(e));
        }
        got += static_cast<std::size_t>(n);
    }
    return got;
}

void send_message(Socket& s, MessageType type, const std::uint8_t* payload, std::size_t len) {
    if (len > MAX_CONTROL_PAYLOAD) {
        throw ProtocolError("outgoing frame exceeds MAX_CONTROL_PAYLOAD");
    }
    std::array<std::uint8_t, HEADER_SIZE> hdr{};
    hdr[0] = static_cast<std::uint8_t>(type);
    const auto n = static_cast<std::uint32_t>(len);
    hdr[1] = static_cast<std::uint8_t>((n >> 24) & 0xFFu);
    hdr[2] = static_cast<std::uint8_t>((n >> 16) & 0xFFu);
    hdr[3] = static_cast<std::uint8_t>((n >> 8) & 0xFFu);
    hdr[4] = static_cast<std::uint8_t>(n & 0xFFu);
    send_all(s, hdr.data(), hdr.size());
    if (len > 0) {
        send_all(s, payload, len);
    }
}

void shutdown_handle(std::intptr_t handle) noexcept {
    if (handle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::shutdown(static_cast<native_socket>(handle), SD_BOTH);
#else
    ::shutdown(static_cast<native_socket>(handle), SHUT_RDWR);
#endif
}

std::optional<Frame> recv_message(Socket& s) {
    std::array<std::uint8_t, HEADER_SIZE> hdr{};
    const std::size_t got = recv_exact(s, hdr.data(), hdr.size());
    if (got == 0) {
        return std::nullopt; // clean close at frame boundary
    }
    if (got < HEADER_SIZE) {
        throw ProtocolError("truncated frame header");
    }
    const FrameHeader header = parse_header(hdr.data());
    Frame frame;
    frame.type = header.type;
    if (header.payload_len > 0) {
        frame.payload.resize(header.payload_len);
        const std::size_t pgot = recv_exact(s, frame.payload.data(), header.payload_len);
        if (pgot < header.payload_len) {
            throw ProtocolError("truncated frame payload");
        }
    }
    return frame;
}

} // namespace fileshare::net
