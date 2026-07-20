#pragma once

// Blocking v2 client transport: connect + handshake + request/response helpers,
// tree browsing, and streaming download with resume. Used by the batch CLI, the
// TUI's RemoteFs, and integration tests. Pure transport -- no UI, no threads of
// its own (the TUI runs it on a dedicated connection thread).

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "fileshare/net.hpp"
#include "fileshare/v2/protocol.hpp"

namespace fileshare::v2 {

// Thrown when the server answers a request with an ERROR frame.
class RemoteError : public std::runtime_error {
public:
    RemoteError(ErrCode code, const std::string& msg)
        : std::runtime_error(msg), code_(code) {}
    [[nodiscard]] ErrCode code() const noexcept { return code_; }
private:
    ErrCode code_;
};

class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    struct ConnectResult {
        bool          ok = false;
        Role          role = Role::ANONYMOUS;
        std::uint64_t session_id = 0;
        std::string   motd;
        std::string   error;      // populated when !ok
        ErrCode       error_code = ErrCode::OK;
    };

    // Full connect: TCP + HELLO + AUTH. In M7 the password is unused (no-auth
    // server); M8 wires it into the challenge-response proof.
    ConnectResult connect(const std::string& host, std::uint16_t port,
                          const std::string& login, const std::string& password);
    void disconnect() noexcept;
    [[nodiscard]] bool connected() const noexcept { return connected_; }

    // Half-close the socket to unblock a blocking recv/send in progress on
    // another thread (used so the UI can stop the connection worker promptly even
    // mid-download). Safe to call from a different thread than the one doing I/O.
    void interrupt() noexcept;

    // --- Filesystem (throw RemoteError on a server ERROR) -------------------
    [[nodiscard]] std::vector<DirEntry> list_dir(const std::string& path);
    [[nodiscard]] DirEntry              stat(const std::string& path);
    [[nodiscard]] ChecksumResponse      checksum(const std::string& path);

    // Subscribe to server push events. Unsolicited EVENT_*/PONG frames that
    // arrive between requests (or during a download) are delivered here.
    void subscribe(std::uint32_t mask);
    void set_event_handler(std::function<void(const Frame&)> handler) {
        event_handler_ = std::move(handler);
    }
    void send_ping();   // fire-and-forget heartbeat; the PONG is an async frame

    enum class PollResult { NONE, EVENT, CLOSED };
    // Read + dispatch any server-pushed frames ready within timeout_ms. Returns
    // EVENT if one was handled, NONE on timeout, CLOSED if the peer hung up.
    PollResult poll_events(int timeout_ms);

    // --- Download -----------------------------------------------------------
    struct DownloadResult {
        bool          ok = false;          // transfer completed
        bool          checksum_ok = false; // verified against server checksum
        bool          resumed = false;
        std::uint64_t bytes = 0;           // total size of the finished file
        std::string   error;
    };
    using ProgressFn = std::function<void(std::uint64_t done, std::uint64_t total)>;

    // Download `remote` to `local`, resuming from `local`.part if present.
    // Renames .part -> local only on a verified (or unverifiable-algo) success.
    DownloadResult download(const std::string& remote, const std::string& local,
                            const ProgressFn& progress = {});

    // --- Admin --------------------------------------------------------------
    [[nodiscard]] std::string                  admin_get_config();
    [[nodiscard]] AdminStats                   admin_stats();
    [[nodiscard]] std::vector<AdminClientInfo> admin_list_clients();
    [[nodiscard]] AdminKickResult              admin_kick(std::uint64_t session_id);
    [[nodiscard]] AdminSetResult               admin_set(const std::string& key, const std::string& value);
    [[nodiscard]] AdminShutdownResult          admin_shutdown(std::uint32_t grace_seconds);

private:
    // Send a request frame, receive the next frame, and translate an ERROR frame
    // into a RemoteError. Returns the (non-error) response frame.
    Frame request(const std::vector<std::uint8_t>& frame, Msg expect);
    Frame recv_expect(Msg expect);

    net::Socket sock_;
    bool        connected_ = false;
    std::function<void(const Frame&)> event_handler_;
};

// True for frames the server may push unsolicited (events + PONG).
[[nodiscard]] bool is_async_frame(Msg m) noexcept;

} // namespace fileshare::v2
