#include "fileshare/v2/client.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "fileshare/checksum.hpp"
#include "fileshare/types.hpp"
#include "fileshare/v2/auth.hpp"
#include "fileshare/v2/wire.hpp"

namespace fs = std::filesystem;

namespace fileshare::v2 {

Client::~Client() { disconnect(); }

bool is_async_frame(Msg m) noexcept {
    return m == Msg::EVENT_FS || m == Msg::EVENT_NOTICE || m == Msg::EVENT_CONFIG ||
           m == Msg::PONG;
}

// --- Low-level request/response --------------------------------------------
Frame Client::recv_expect(Msg expect) {
    // Loop so server-pushed events (or a PONG) that arrive before the expected
    // reply are dispatched out of band rather than mistaken for the response.
    for (;;) {
        std::optional<Frame> fr = recv_frame(sock_);
        if (!fr) {
            connected_ = false;
            throw RemoteError(ErrCode::INTERNAL_ERROR, "connection closed by server");
        }
        if (is_async_frame(fr->type)) {
            if (event_handler_) event_handler_(*fr);
            continue;
        }
        if (fr->type == Msg::ERROR_MSG) {
            const ErrorMessage e = parse_error(fr->payload.data(), fr->payload.size());
            throw RemoteError(e.code, e.message);
        }
        if (fr->type != expect) {
            throw RemoteError(ErrCode::INTERNAL_ERROR,
                              std::string("unexpected reply: ") + msg_name(fr->type));
        }
        return std::move(*fr);
    }
}

Client::PollResult Client::poll_events(int timeout_ms) {
    if (!connected_) return PollResult::CLOSED;
    if (!net::wait_readable(sock_, timeout_ms)) return PollResult::NONE;
    std::optional<Frame> fr = recv_frame(sock_);
    if (!fr) { connected_ = false; return PollResult::CLOSED; }
    // When idle we only expect server-pushed frames; deliver those to the event
    // handler and ignore anything else rather than mislabelling it as an event.
    if (is_async_frame(fr->type) && event_handler_) event_handler_(*fr);
    return PollResult::EVENT;
}

void Client::send_ping() {
    send_frame(sock_, encode_ping());   // PONG comes back as an async frame
}

Frame Client::request(const std::vector<std::uint8_t>& frame, Msg expect) {
    send_frame(sock_, frame);
    return recv_expect(expect);
}

// --- Connect / handshake ----------------------------------------------------
Client::ConnectResult Client::connect(const std::string& host, std::uint16_t port,
                                      const std::string& login, const std::string& password) {
    ConnectResult res;
    try {
        sock_ = net::tcp_connect(host, port);
        connected_ = true;

        Hello hello;
        hello.proto_ver = PROTO_VERSION;
        hello.client_name = "commander/2.0";
        Frame hok = request(encode_hello(hello), Msg::HELLO_OK);
        const HelloOk ok = parse_hello_ok(hok.payload.data(), hok.payload.size());
        if (ok.proto_ver != PROTO_VERSION) {
            res.error = "server speaks a different protocol version";
            res.error_code = ErrCode::UNSUPPORTED_VERSION;
            disconnect();
            return res;
        }

        AuthRequest areq;
        areq.login = login;
        if (ok.auth_mode == AUTH_MODE_CHALLENGE) {
            // Challenge-response: derive the proof from the password without
            // sending it. No-auth mode leaves the proof zero-filled.
            areq.proof = compute_client_proof(password, login, ok.challenge, ok.pbkdf2_iters);
        }
        send_frame(sock_, encode_auth_request(areq));

        std::optional<Frame> af = recv_frame(sock_);
        if (!af) { res.error = "connection closed during auth"; disconnect(); return res; }
        if (af->type == Msg::AUTH_FAIL) {
            const AuthFail f = parse_auth_fail(af->payload.data(), af->payload.size());
            res.error = f.message;
            res.error_code = ErrCode::AUTH_FAILED;
            disconnect();
            return res;
        }
        if (af->type == Msg::ERROR_MSG) {
            const ErrorMessage e = parse_error(af->payload.data(), af->payload.size());
            res.error = e.message;
            res.error_code = e.code;
            disconnect();
            return res;
        }
        if (af->type != Msg::AUTH_OK) {
            res.error = "unexpected reply during auth";
            disconnect();
            return res;
        }
        const AuthOk aok = parse_auth_ok(af->payload.data(), af->payload.size());
        res.ok = true;
        res.role = aok.role;
        res.session_id = aok.session_id;
        res.motd = aok.motd;
        return res;
    } catch (const net::NetError& e) {
        res.error = e.what();
        disconnect();
        return res;
    } catch (const ProtocolError& e) {
        res.error = e.what();
        disconnect();
        return res;
    } catch (const RemoteError& e) {
        res.error = e.what();
        res.error_code = e.code();
        disconnect();
        return res;
    }
}

void Client::disconnect() noexcept {
    if (connected_) {
        sock_.close();
        connected_ = false;
    }
}

void Client::interrupt() noexcept {
    // Only shuts down the socket (does not close/reuse the fd), so a peer thread
    // blocked in recv/send wakes with an error. handle_ is set at connect and
    // not mutated during the session, so this cross-thread read is safe.
    if (connected_) net::shutdown_handle(sock_.handle());
}

// --- Filesystem -------------------------------------------------------------
std::vector<DirEntry> Client::list_dir(const std::string& path) {
    const Frame f = request(encode_list_dir_request(ListDirRequest{path}), Msg::LIST_DIR_RESPONSE);
    return parse_list_dir_response(f.payload.data(), f.payload.size()).entries;
}

DirEntry Client::stat(const std::string& path) {
    const Frame f = request(encode_stat_request(StatRequest{path}), Msg::STAT_RESPONSE);
    return parse_stat_response(f.payload.data(), f.payload.size()).entry;
}

ChecksumResponse Client::checksum(const std::string& path) {
    const Frame f = request(encode_checksum_request(ChecksumRequest{path}), Msg::CHECKSUM_RESPONSE);
    return parse_checksum_response(f.payload.data(), f.payload.size());
}

void Client::subscribe(std::uint32_t mask) {
    send_frame(sock_, encode_subscribe(Subscribe{mask}));   // fire-and-forget
}

// --- Download ---------------------------------------------------------------
namespace {
std::uint8_t algo_code_of(const std::string& algo) {
    return algo == "sha256" ? ALGO_SHA256 : ALGO_CRC32;
}
} // namespace

Client::DownloadResult Client::download(const std::string& remote, const std::string& local,
                                        const ProgressFn& progress) {
    DownloadResult res;
    const std::string part = local + ".part";

    std::uint64_t offset = 0;
    std::error_code ec;
    if (fs::exists(part, ec)) {
        offset = fs::file_size(part, ec);
        if (ec) offset = 0;
        res.resumed = offset > 0;
    }

    try {
        // Negotiate the transfer. If the remote file shrank below our resume
        // offset, the server rejects it (UNSUPPORTED_OFFSET); drop the stale
        // .part and retry from the start instead of getting stuck forever.
        DownloadAccept accept{};
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                Frame acc = request(encode_download_request(DownloadRequest{remote, offset}),
                                    Msg::DOWNLOAD_ACCEPT);
                accept = parse_download_accept(acc.payload.data(), acc.payload.size());
                break;
            } catch (const RemoteError& e) {
                if (e.code() == ErrCode::UNSUPPORTED_OFFSET && offset > 0) {
                    fs::remove(part, ec);
                    offset = 0;
                    res.resumed = false;
                    continue;   // retry from scratch
                }
                throw;
            }
        }
        const std::uint64_t total = accept.total_size;
        res.bytes = total;

        // Open the .part file: append when resuming, truncate otherwise.
        std::ofstream out(part, offset > 0 ? (std::ios::binary | std::ios::app)
                                           : (std::ios::binary | std::ios::trunc));
        if (!out) {
            res.error = "cannot open local file: " + part;
            return res;
        }

        std::uint64_t done = offset;
        if (progress) progress(done, total);

        DownloadDone done_msg;
        bool got_done = false;
        while (!got_done) {
            std::optional<Frame> fr = recv_frame(sock_);
            if (!fr) { connected_ = false; res.error = "connection closed mid-transfer"; return res; }
            if (is_async_frame(fr->type)) {   // event pushed during the transfer
                if (event_handler_) event_handler_(*fr);
                continue;
            }
            switch (fr->type) {
                case Msg::CHUNK_DATA: {
                    const ChunkView v = parse_chunk_data(fr->payload.data(), fr->payload.size());
                    // Guard against a buggy/hostile server streaming past the
                    // declared size and filling the disk.
                    if (done + v.len > total) {
                        res.error = "server sent more data than declared";
                        return res;
                    }
                    out.write(reinterpret_cast<const char*>(v.data),
                              static_cast<std::streamsize>(v.len));
                    done += v.len;
                    if (progress) progress(done, total);
                    break;
                }
                case Msg::DOWNLOAD_DONE:
                    done_msg = parse_download_done(fr->payload.data(), fr->payload.size());
                    got_done = true;
                    break;
                case Msg::ERROR_MSG: {
                    const ErrorMessage e = parse_error(fr->payload.data(), fr->payload.size());
                    res.error = e.message;
                    return res;   // keep .part for a later resume
                }
                default:
                    res.error = std::string("unexpected frame during transfer: ") + msg_name(fr->type);
                    return res;
            }
        }
        out.close();
        if (!out) {   // flush/close error -> the .part is not trustworthy
            res.error = "write error finalizing " + part;
            return res;
        }

        // Verify against the server's full-file checksum.
        const FileDigest digest = compute_file_digest(part);
        if (digest.ok && algo_code_of(digest.algo) == done_msg.algo) {
            res.checksum_ok = (digest.checksum == done_msg.checksum);
            if (!res.checksum_ok) {
                res.error = "checksum mismatch";
                // The .part is corrupt (its bytes don't match the server). Drop
                // it so a retry re-downloads from scratch -- otherwise a full-size
                // corrupt .part would resume at offset==total forever and keep
                // re-failing verification on the same bad bytes.
                fs::remove(part, ec);
                return res;
            }
        } else {
            // Different algo build than the server: transfer is complete but
            // unverifiable here. Accept it, flag checksum as not verified.
            res.checksum_ok = false;
        }

        // Success: atomically move .part -> final name. Only report success if
        // the file actually lands -- otherwise surface the error and keep .part.
        fs::rename(part, local, ec);
        if (ec) {
            std::error_code cec;
            fs::copy_file(part, local, fs::copy_options::overwrite_existing, cec);
            if (cec) {
                res.error = "cannot place downloaded file: " + cec.message();
                return res;   // ok stays false; .part preserved
            }
            fs::remove(part, cec);
        }
        res.ok = true;
        return res;
    } catch (const RemoteError& e) {
        res.error = e.what();
        return res;
    } catch (const net::NetError& e) {
        connected_ = false;
        res.error = e.what();
        return res;
    } catch (const ProtocolError& e) {
        res.error = e.what();
        return res;
    }
}

// --- Admin ------------------------------------------------------------------
std::string Client::admin_get_config() {
    const Frame f = request(encode_admin_get_config(), Msg::ADMIN_CONFIG);
    return parse_admin_config(f.payload.data(), f.payload.size());
}

AdminStats Client::admin_stats() {
    const Frame f = request(encode_admin_stats(), Msg::ADMIN_STATS_RESPONSE);
    return parse_admin_stats_response(f.payload.data(), f.payload.size());
}

std::vector<AdminClientInfo> Client::admin_list_clients() {
    const Frame f = request(encode_admin_list_clients(), Msg::ADMIN_CLIENTS);
    return parse_admin_clients(f.payload.data(), f.payload.size());
}

AdminKickResult Client::admin_kick(std::uint64_t session_id) {
    const Frame f = request(encode_admin_kick(AdminKick{session_id}), Msg::ADMIN_KICK_RESULT);
    return parse_admin_kick_result(f.payload.data(), f.payload.size());
}

AdminSetResult Client::admin_set(const std::string& key, const std::string& value) {
    const Frame f = request(encode_admin_set(AdminSet{key, value}), Msg::ADMIN_SET_RESULT);
    return parse_admin_set_result(f.payload.data(), f.payload.size());
}

AdminShutdownResult Client::admin_shutdown(std::uint32_t grace_seconds) {
    const Frame f = request(encode_admin_shutdown(AdminShutdown{grace_seconds}),
                            Msg::ADMIN_SHUTDOWN_RESULT);
    return parse_admin_shutdown_result(f.payload.data(), f.payload.size());
}

} // namespace fileshare::v2
