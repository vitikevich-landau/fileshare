#include "fileshare/epoll_coro_server.hpp"

#ifdef __linux__

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <fstream>
#include <utility>

#include "fileshare/checksum.hpp"
#include "fileshare/net.hpp"
#include "fileshare/protocol.hpp"

namespace fileshare {

// --- Coroutine return type: a lazy, self-owned connection coroutine ----------
struct CoroServer::Task {
    struct promise_type {
        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; } // reactor starts it
        std::suspend_always final_suspend() noexcept { return {}; }   // keep frame until Conn dies
        void return_void() noexcept {}
        void unhandled_exception() noexcept {} // an escaped throw just drops the connection
    };
    using handle_type = std::coroutine_handle<promise_type>;

    handle_type handle{};

    Task() noexcept = default;
    explicit Task(handle_type h) noexcept : handle(h) {}
    Task(Task&& other) noexcept : handle(std::exchange(other.handle, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle) {
                handle.destroy();
            }
            handle = std::exchange(other.handle, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() {
        if (handle) {
            handle.destroy();
        }
    }
    [[nodiscard]] bool done() const noexcept { return handle && handle.done(); }
};

// --- Per-connection state (single reactor thread -> no locking) --------------
struct CoroServer::Conn {
    int                          fd = -1;
    std::shared_ptr<ClientEntry> entry;
    std::coroutine_handle<>      waiting{}; // handle to resume on the next event
    bool                         in_epoll = false;
    bool                         pending_close = false; // arm() failed -> close after resume
    Task                         task; // owns the coroutine frame
};

namespace {

constexpr std::size_t kMaxInbuf = 8 * 1024 * 1024;

std::string strerr(int e) { return std::strerror(e); }

std::optional<Frame> try_take_frame(std::vector<std::uint8_t>& buf) {
    if (buf.size() < HEADER_SIZE) {
        return std::nullopt;
    }
    const FrameHeader header = parse_header(buf.data());
    const std::size_t total = HEADER_SIZE + header.payload_len;
    if (buf.size() < total) {
        return std::nullopt;
    }
    Frame frame;
    frame.type = header.type;
    frame.payload.assign(buf.begin() + static_cast<std::ptrdiff_t>(HEADER_SIZE),
                         buf.begin() + static_cast<std::ptrdiff_t>(total));
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(total));
    return frame;
}

// Awaiter: suspend the connection coroutine until `fd` is ready for `events`.
struct IoAwaiter {
    CoroServer*   server;
    int           fd;
    std::uint32_t events;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const noexcept { server->arm(fd, events, h); }
    void await_resume() const noexcept {}
};

} // namespace

CoroServer::CoroServer(Catalog catalog, std::string config_path, std::chrono::milliseconds drain_grace)
    : core_(std::move(catalog), std::move(config_path)), drain_grace_(drain_grace) {}

CoroServer::~CoroServer() {
    while (!conns_.empty()) {
        close_conn(conns_.begin()->first);
    }
    if (listener_fd_ >= 0) {
        ::close(listener_fd_);
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

std::uint16_t CoroServer::listen(std::uint16_t port) {
    net::startup();
    listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listener_fd_ < 0) {
        throw net::NetError("socket failed: " + strerr(errno));
    }
    int yes = 1;
    ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listener_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int e = errno;
        ::close(listener_fd_);
        listener_fd_ = -1;
        throw net::NetError("bind failed: " + strerr(e));
    }
    if (::listen(listener_fd_, 128) != 0) {
        const int e = errno;
        ::close(listener_fd_);
        listener_fd_ = -1;
        throw net::NetError("listen failed: " + strerr(e));
    }
    std::uint16_t bound = port;
    sockaddr_in got{};
    socklen_t got_len = sizeof(got);
    if (::getsockname(listener_fd_, reinterpret_cast<sockaddr*>(&got), &got_len) == 0) {
        bound = ntohs(got.sin_port);
    }
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        const int e = errno;
        ::close(listener_fd_);
        listener_fd_ = -1;
        throw net::NetError("epoll_create1 failed: " + strerr(e));
    }
    epoll_event ev{};
    // Edge-triggered: accept_new() drains the backlog to EAGAIN, and an accept
    // error we cannot recover from (e.g. EMFILE) will not re-fire on a still-
    // readable listener and spin the reactor at 100% CPU.
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listener_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listener_fd_, &ev) != 0) {
        const int e = errno;
        ::close(epoll_fd_);
        ::close(listener_fd_);
        epoll_fd_ = -1;
        listener_fd_ = -1;
        throw net::NetError("epoll_ctl(listener) failed: " + strerr(e));
    }
    core_.mark_started();
    return bound;
}

void CoroServer::arm(int fd, std::uint32_t events, std::coroutine_handle<> h) {
    const auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    it->second->waiting = h;
    epoll_event ev{};
    ev.events = events | EPOLLONESHOT;
    ev.data.fd = fd;
    const int op = it->second->in_epoll ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(epoll_fd_, op, fd, &ev) != 0) {
        // The fd cannot be watched (e.g. max_user_watches / ENOMEM), so no event
        // will ever arrive to resume this coroutine. Flag it so the reactor
        // closes the connection once this resume unwinds (we must not destroy the
        // frame from inside its own await_suspend).
        it->second->pending_close = true;
        return;
    }
    it->second->in_epoll = true;
}

void CoroServer::accept_new() {
    for (;;) {
        sockaddr_in addr{};
        socklen_t addr_len = sizeof(addr);
        const int fd =
            ::accept4(listener_fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            break;
        }
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        std::string peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));

        auto conn = std::make_unique<Conn>();
        conn->fd = fd;
        conn->entry = core_.registry().add(std::move(peer), static_cast<std::intptr_t>(fd));
        conn->task = handle_connection(fd, conn->entry);
        Conn* raw = conn.get();
        conns_.emplace(fd, std::move(conn));

        raw->task.handle.resume(); // start the coroutine (runs to its first co_await or return)
        if (raw->task.done() || raw->pending_close) {
            close_conn(fd);
        }
    }
}

void CoroServer::resume_fd(int fd) {
    const auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    const std::coroutine_handle<> h = it->second->waiting;
    if (!h) {
        return;
    }
    it->second->waiting = {};
    h.resume(); // runs the connection coroutine to its next co_await or co_return
    const auto jt = conns_.find(fd);
    if (jt != conns_.end() && (jt->second->task.done() || jt->second->pending_close)) {
        close_conn(fd);
    }
}

void CoroServer::close_conn(int fd) {
    const auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    if (it->second->entry) {
        core_.registry().remove(it->second->entry->id()); // deregister before closing the fd
    }
    if (it->second->in_epoll) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    }
    ::close(fd);
    conns_.erase(it); // destroys Conn -> Task -> the coroutine frame
}

void CoroServer::pump_events(int timeout_ms) {
    std::array<epoll_event, 64> events{};
    const int n = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), timeout_ms);
    if (n < 0) {
        return; // EINTR or a transient error: the caller loops again
    }
    for (int i = 0; i < n; ++i) {
        const int fd = events[i].data.fd;
        if (fd == listener_fd_) {
            accept_new();
        } else {
            resume_fd(fd);
        }
    }
}

void CoroServer::serve_forever() {
    // --- Normal phase ------------------------------------------------------
    while (core_.accepting()) {
        core_.drain_commands();
        pump_events(200);
    }

    // --- Graceful drain ----------------------------------------------------
    if (listener_fd_ >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listener_fd_, nullptr); // stop accepting
    }
    core_.registry().shutdown_idle();
    const auto deadline = std::chrono::steady_clock::now() + drain_grace_;
    while (!conns_.empty() && std::chrono::steady_clock::now() < deadline) {
        core_.drain_commands();
        pump_events(100);
    }
    core_.registry().shutdown_all();
    const auto hard_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!conns_.empty() && std::chrono::steady_clock::now() < hard_deadline) {
        pump_events(50);
    }

    // Force-destroy anything still lingering (frames suspended mid-transfer are
    // destroyed cleanly by close_conn -> Task -> handle.destroy()).
    while (!conns_.empty()) {
        close_conn(conns_.begin()->first);
    }
    if (listener_fd_ >= 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

// --- The per-connection coroutine -------------------------------------------
// Linear code: read a request, send a response / stream a file, repeat. Each
// blocking point is a co_await on the socket becoming readable/writable.
CoroServer::Task CoroServer::handle_connection(int fd, std::shared_ptr<ClientEntry> entry) {
    std::vector<std::uint8_t> inbuf;
    std::array<std::uint8_t, 64 * 1024> tmp{};
    std::vector<std::uint8_t> outbuf;
    std::size_t out_pos = 0;

    bool          downloading = false;
    std::ifstream file;
    std::uint64_t remaining = 0;
    Checksum      done_checksum{};

    try {
        for (;;) {
            // === Flush any pending output (co_await writable on backpressure) ===
            while (out_pos < outbuf.size()) {
                const ssize_t n = ::send(fd, outbuf.data() + out_pos, outbuf.size() - out_pos,
                                         MSG_NOSIGNAL);
                if (n > 0) {
                    out_pos += static_cast<std::size_t>(n);
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    co_await IoAwaiter{this, fd, EPOLLOUT};
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                co_return; // fatal write error / peer gone
            }

            // === Mid-download: load the next chunk (or the DONE trailer) ===
            if (downloading) {
                outbuf.clear();
                out_pos = 0;
                if (remaining == 0) {
                    outbuf = encode_download_done(done_checksum);
                    downloading = false;
                    file.close();
                    core_.inc_completed();
                } else {
                    const std::size_t want =
                        static_cast<std::size_t>(std::min<std::uint64_t>(remaining, CHUNK_SIZE));
                    std::vector<char> fbuf(want);
                    file.read(fbuf.data(), static_cast<std::streamsize>(want));
                    const std::streamsize got = file.gcount();
                    if (got <= 0) {
                        remaining = 0;
                        outbuf = encode_download_done(done_checksum);
                        downloading = false;
                        file.close();
                        core_.inc_completed();
                    } else {
                        outbuf = encode_chunk_data(reinterpret_cast<const std::uint8_t*>(fbuf.data()),
                                                   static_cast<std::size_t>(got));
                        remaining -= static_cast<std::uint64_t>(got);
                        core_.add_bytes(static_cast<std::uint64_t>(got));
                        if (entry) {
                            entry->add_bytes(static_cast<std::uint64_t>(got));
                        }
                    }
                }
                continue; // loop back to send the freshly loaded frame
            }

            // === Idle: the DONE (if any) has been flushed -> mark not-downloading ===
            if (entry) {
                entry->set_alias("");
            }

            // === Read the next request (co_await readable when the socket blocks) ===
            std::optional<Frame> frame;
            while (!frame) {
                if (auto f = try_take_frame(inbuf)) {
                    frame = std::move(f);
                    break;
                }
                const ssize_t n = ::recv(fd, tmp.data(), tmp.size(), 0);
                if (n > 0) {
                    inbuf.insert(inbuf.end(), tmp.data(), tmp.data() + n);
                    if (inbuf.size() > kMaxInbuf) {
                        co_return; // abuse guard
                    }
                    continue;
                }
                if (n == 0) {
                    co_return; // clean close
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    co_await IoAwaiter{this, fd, EPOLLIN};
                    continue;
                }
                if (errno == EINTR) {
                    continue;
                }
                co_return; // fatal
            }

            // === Dispatch: fill outbuf with a control reply or start a download ===
            switch (frame->type) {
                case MessageType::LIST_REQUEST:
                    outbuf = core_.build_list_response();
                    out_pos = 0;
                    break;
                case MessageType::PING:
                    outbuf = encode_pong();
                    out_pos = 0;
                    break;
                case MessageType::DOWNLOAD_REQUEST: {
                    const DownloadRequest req =
                        parse_download_request(frame->payload.data(), frame->payload.size());
                    const std::optional<SharedFileEntry> ent = core_.resolve_download(req.alias);
                    if (!ent) {
                        outbuf = encode_error(
                            ErrorMessage{ErrorCode::FILE_NOT_FOUND, "no such alias: " + req.alias});
                        out_pos = 0;
                        break;
                    }
                    if (req.offset > ent->size_bytes) {
                        outbuf = encode_error(
                            ErrorMessage{ErrorCode::UNSUPPORTED_OFFSET, "offset beyond end of file"});
                        out_pos = 0;
                        break;
                    }
                    file.close();
                    file.clear();
                    file.open(ent->path, std::ios::binary);
                    if (!file) {
                        outbuf = encode_error(
                            ErrorMessage{ErrorCode::INTERNAL_ERROR, "cannot open file on server"});
                        out_pos = 0;
                        break;
                    }
                    if (req.offset > 0) {
                        file.seekg(static_cast<std::streamoff>(req.offset));
                    }
                    downloading = true;
                    remaining = ent->size_bytes - req.offset;
                    done_checksum = ent->checksum;
                    if (entry) {
                        entry->set_alias(req.alias);
                    }
                    outbuf.clear();
                    out_pos = 0; // first chunk is loaded at the top of the loop
                    break;
                }
                default:
                    co_return; // unexpected type -> drop
            }
        }
    } catch (const ProtocolError&) {
        // malformed frame/payload -> drop this connection only
    } catch (...) {
        // anything else -> drop this connection only
    }

    if (entry) {
        entry->set_alias(""); // in case we died mid-download
    }
    co_return;
}

} // namespace fileshare

#endif // __linux__
