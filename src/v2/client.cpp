#include "fileshare/v2/client.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "fileshare/checksum.hpp"
#include "fileshare/types.hpp"
#include "fileshare/v2/wire.hpp"

namespace fs = std::filesystem;

namespace fileshare::v2 {

Client::~Client() { disconnect(); }

// --- Low-level request/response --------------------------------------------
Frame Client::recv_expect(Msg expect) {
    std::optional<Frame> fr = recv_frame(sock_);
    if (!fr) {
        connected_ = false;
        throw RemoteError(ErrCode::INTERNAL_ERROR, "connection closed by server");
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

Frame Client::request(const std::vector<std::uint8_t>& frame, Msg expect) {
    send_frame(sock_, frame);
    return recv_expect(expect);
}

// --- Connect / handshake ----------------------------------------------------
Client::ConnectResult Client::connect(const std::string& host, std::uint16_t port,
                                      const std::string& login, const std::string& /*password*/) {
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

        // M7: no-auth server ignores the proof; M8 computes it from `password`
        // and ok.challenge.
        AuthRequest areq;
        areq.login = login;
        // proof stays zero-filled in M7.
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

void Client::ping() {
    (void)request(encode_ping(), Msg::PONG);
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
        Frame acc = request(encode_download_request(DownloadRequest{remote, offset}),
                            Msg::DOWNLOAD_ACCEPT);
        const DownloadAccept accept = parse_download_accept(acc.payload.data(), acc.payload.size());
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
            switch (fr->type) {
                case Msg::CHUNK_DATA: {
                    const ChunkView v = parse_chunk_data(fr->payload.data(), fr->payload.size());
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

        // Verify against the server's full-file checksum.
        const FileDigest digest = compute_file_digest(part);
        if (digest.ok && algo_code_of(digest.algo) == done_msg.algo) {
            res.checksum_ok = (digest.checksum == done_msg.checksum);
            if (!res.checksum_ok) {
                res.error = "checksum mismatch";
                return res;    // keep .part; caller may retry
            }
        } else {
            // Different algo build than the server: transfer is complete but
            // unverifiable here. Accept it, flag checksum as not verified.
            res.checksum_ok = false;
        }

        // Success: atomically move .part -> final name.
        fs::rename(part, local, ec);
        if (ec) {
            // Fall back to copy+remove across filesystems.
            fs::copy_file(part, local, fs::copy_options::overwrite_existing, ec);
            fs::remove(part, ec);
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
