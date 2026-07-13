#include "fileshare/epoll_server.hpp"

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
#include <fstream>
#include <thread>

#include "fileshare/checksum.hpp"
#include "fileshare/net.hpp"

namespace fileshare {

// Per-connection state, owned by exactly one worker at a time (EPOLLONESHOT).
struct EpollConn {
    int                          fd = -1;
    std::shared_ptr<ClientEntry> entry;

    std::vector<std::uint8_t> inbuf;   // received but not-yet-parsed bytes
    std::vector<std::uint8_t> outbuf;  // pending output
    std::size_t               out_pos = 0;

    bool          downloading = false;
    std::ifstream file;
    std::uint64_t remaining = 0;
    Checksum      done_checksum{};
};

namespace {

constexpr std::size_t kMaxInbuf          = 8 * 1024 * 1024; // abuse guard
constexpr std::size_t kReadCapPerCall    = 4 * 1024 * 1024; // fairness
constexpr int         kWriteChunkBudget  = 32;              // chunks per do_write (fairness)
constexpr int         kMaxEvents         = 64;

std::string strerr(int e) { return std::strerror(e); }

// Peel one complete frame off the front of `buf`, or nullopt if incomplete.
// Throws ProtocolError on an unknown type / oversize payload.
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

} // namespace

EpollServer::EpollServer(Catalog catalog, std::string config_path, std::size_t workers)
    : core_(std::move(catalog), std::move(config_path)), worker_count_(workers) {
    if (worker_count_ == 0) {
        const unsigned hc = std::thread::hardware_concurrency();
        worker_count_ = hc > 0 ? hc : 4;
    }
}

EpollServer::~EpollServer() {
    if (pool_) {
        pool_->stop();
    }
    for (auto& kv : conns_) {
        if (kv.second->fd >= 0) {
            ::close(kv.second->fd);
        }
    }
    conns_.clear();
    if (listener_fd_ >= 0) {
        ::close(listener_fd_);
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

std::uint16_t EpollServer::listen(std::uint16_t port) {
    net::startup(); // ignore SIGPIPE

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
    ev.events = EPOLLIN; // listener is level-triggered; accept_new() drains it
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

void EpollServer::serve_forever() {
    pool_ = std::make_unique<ThreadPool>(worker_count_);
    std::array<epoll_event, kMaxEvents> events{};

    while (core_.running()) {
        core_.drain_commands();
        const int n = ::epoll_wait(epoll_fd_, events.data(), kMaxEvents, 200);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; // fatal epoll error
        }
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == listener_fd_) {
                accept_new();
            } else {
                auto conn = lookup(fd);
                if (conn) {
                    const std::uint32_t ev = events[i].events;
                    pool_->submit([this, conn, ev] { process(conn, ev); });
                }
            }
        }
    }

    // Teardown: stop the pool (drains in-flight tasks + joins) so no worker
    // touches a connection afterwards, then close everything.
    pool_->stop();
    pool_.reset();
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        for (auto& kv : conns_) {
            if (kv.second->entry) {
                core_.registry().remove(kv.second->entry->id());
            }
            if (kv.second->fd >= 0) {
                ::close(kv.second->fd);
            }
        }
        conns_.clear();
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

void EpollServer::accept_new() {
    for (;;) {
        sockaddr_in addr{};
        socklen_t addr_len = sizeof(addr);
        const int fd =
            ::accept4(listener_fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // drained
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            break; // other error
        }

        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        std::string peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));

        auto conn = std::make_shared<EpollConn>();
        conn->fd = fd;
        conn->entry = core_.registry().add(std::move(peer), static_cast<std::intptr_t>(fd));
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            conns_[fd] = conn;
        }

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            close_conn(conn);
        }
    }
}

std::shared_ptr<EpollConn> EpollServer::lookup(int fd) {
    std::lock_guard<std::mutex> lock(conns_mutex_);
    const auto it = conns_.find(fd);
    return it == conns_.end() ? nullptr : it->second;
}

void EpollServer::process(std::shared_ptr<EpollConn> conn, std::uint32_t events) {
    bool ok = true;
    try {
        if (events & (EPOLLERR | EPOLLHUP)) {
            ok = false;
        } else {
            if (events & EPOLLIN) {
                ok = do_read(*conn);
            }
            if (ok) {
                dispatch_available(*conn); // parse buffered requests if idle
            }
            if (ok && (conn->out_pos < conn->outbuf.size() || conn->downloading)) {
                ok = do_write(*conn);
            }
            if (ok) {
                dispatch_available(*conn); // pick up a pipelined request after draining
            }
        }
    } catch (const ProtocolError&) {
        ok = false; // malformed -> drop this connection (§7)
    } catch (const net::NetError&) {
        ok = false;
    } catch (...) {
        ok = false;
    }

    if (!ok) {
        close_conn(conn);
        return;
    }
    if (!rearm(*conn)) {
        close_conn(conn);
    }
}

bool EpollServer::do_read(EpollConn& conn) {
    std::array<std::uint8_t, 64 * 1024> tmp{};
    std::size_t total = 0;
    for (;;) {
        const ssize_t n = ::recv(conn.fd, tmp.data(), tmp.size(), 0);
        if (n > 0) {
            conn.inbuf.insert(conn.inbuf.end(), tmp.data(), tmp.data() + n);
            if (conn.inbuf.size() > kMaxInbuf) {
                throw ProtocolError("input buffer overflow");
            }
            total += static_cast<std::size_t>(n);
            if (total >= kReadCapPerCall) {
                break; // yield; more will be read on the next EPOLLIN
            }
            continue;
        }
        if (n == 0) {
            // Peer closed its write side. We drop the connection even if inbuf
            // holds a complete frame: our client always keeps the socket open to
            // read the reply, so an EOF here means the client is going away.
            // (Servicing a buffered request before closing would be a later
            // hardening; it is intentionally out of scope here.)
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return false; // fatal
    }
    return true;
}

void EpollServer::dispatch_available(EpollConn& conn) {
    // Only pull a new request while idle: not mid-download and output drained.
    while (!conn.downloading && conn.out_pos >= conn.outbuf.size()) {
        auto frame = try_take_frame(conn.inbuf);
        if (!frame) {
            break;
        }
        switch (frame->type) {
            case MessageType::LIST_REQUEST:
                conn.outbuf = core_.build_list_response();
                conn.out_pos = 0;
                break;
            case MessageType::DOWNLOAD_REQUEST: {
                const DownloadRequest req =
                    parse_download_request(frame->payload.data(), frame->payload.size());
                start_download(conn, req);
                break;
            }
            case MessageType::PING:
                conn.outbuf = encode_pong();
                conn.out_pos = 0;
                break;
            default:
                throw ProtocolError("unexpected message type");
        }
    }
}

void EpollServer::start_download(EpollConn& conn, const DownloadRequest& req) {
    const std::optional<SharedFileEntry> entry = core_.resolve_download(req.alias);
    if (!entry) {
        conn.outbuf = encode_error(ErrorMessage{ErrorCode::FILE_NOT_FOUND, "no such alias: " + req.alias});
        conn.out_pos = 0;
        return;
    }
    if (req.offset > entry->size_bytes) {
        conn.outbuf = encode_error(ErrorMessage{ErrorCode::UNSUPPORTED_OFFSET, "offset beyond end of file"});
        conn.out_pos = 0;
        return;
    }
    conn.file.close();
    conn.file.clear();
    conn.file.open(entry->path, std::ios::binary);
    if (!conn.file) {
        conn.outbuf = encode_error(ErrorMessage{ErrorCode::INTERNAL_ERROR, "cannot open file on server"});
        conn.out_pos = 0;
        return;
    }
    if (req.offset > 0) {
        conn.file.seekg(static_cast<std::streamoff>(req.offset));
    }
    conn.downloading = true;
    conn.remaining = entry->size_bytes - req.offset;
    conn.done_checksum = entry->checksum;
    if (conn.entry) {
        conn.entry->set_alias(req.alias);
    }
    conn.outbuf.clear();
    conn.out_pos = 0;
    refill_download(conn); // prime the first chunk
}

void EpollServer::refill_download(EpollConn& conn) {
    conn.outbuf.clear();
    conn.out_pos = 0;

    auto finish = [&] {
        conn.outbuf = encode_download_done(conn.done_checksum);
        conn.downloading = false;
        conn.remaining = 0;
        conn.file.close();
        if (conn.entry) {
            conn.entry->set_alias("");
        }
        core_.inc_completed();
    };

    if (conn.remaining == 0) {
        finish();
        return;
    }
    const std::size_t want =
        static_cast<std::size_t>(std::min<std::uint64_t>(conn.remaining, CHUNK_SIZE));
    std::vector<char> data(want);
    conn.file.read(data.data(), static_cast<std::streamsize>(want));
    const std::streamsize got = conn.file.gcount();
    if (got <= 0) {
        finish(); // short/failed read: end now; the client's checksum flags corruption
        return;
    }
    conn.outbuf = encode_chunk_data(reinterpret_cast<const std::uint8_t*>(data.data()),
                                    static_cast<std::size_t>(got));
    conn.remaining -= static_cast<std::uint64_t>(got);
    core_.add_bytes(static_cast<std::uint64_t>(got));
    if (conn.entry) {
        conn.entry->add_bytes(static_cast<std::uint64_t>(got));
    }
}

bool EpollServer::do_write(EpollConn& conn) {
    int budget = kWriteChunkBudget;
    for (;;) {
        while (conn.out_pos < conn.outbuf.size()) {
            const ssize_t n = ::send(conn.fd, conn.outbuf.data() + conn.out_pos,
                                     conn.outbuf.size() - conn.out_pos, MSG_NOSIGNAL);
            if (n > 0) {
                conn.out_pos += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return true; // socket full; resume on the next EPOLLOUT
                }
                if (errno == EINTR) {
                    continue;
                }
                return false; // fatal
            }
            return false; // n == 0 unexpected
        }
        if (conn.downloading) {
            if (--budget < 0) {
                return true; // yield for fairness; still downloading
            }
            refill_download(conn);
            if (conn.outbuf.empty()) {
                return true;
            }
            continue;
        }
        return true; // idle, output drained
    }
}

bool EpollServer::rearm(EpollConn& conn) {
    epoll_event ev{};
    ev.data.fd = conn.fd;
    const bool want_write = (conn.out_pos < conn.outbuf.size()) || conn.downloading;
    ev.events = (want_write ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn.fd, &ev) == 0;
}

void EpollServer::close_conn(const std::shared_ptr<EpollConn>& conn) {
    // Deregister before closing the fd (kick / fd-reuse safety), drop from epoll
    // and the map, then close. The task's shared_ptr keeps `conn` alive until it
    // returns, so no other thread can be mid-use here (EPOLLONESHOT).
    if (conn->entry) {
        core_.registry().remove(conn->entry->id());
    }
    if (conn->fd >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn->fd, nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conns_.erase(conn->fd);
    }
    if (conn->fd >= 0) {
        ::close(conn->fd);
        conn->fd = -1;
    }
}

} // namespace fileshare

#endif // __linux__
