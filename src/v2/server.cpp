#include "fileshare/v2/server.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

#include "fileshare/types.hpp"
#include "fileshare/v2/auth.hpp"
#include "fileshare/v2/crypto.hpp"
#include "fileshare/v2/dispatch.hpp"
#include "fileshare/v2/log.hpp"
#include "fileshare/v2/vfs.hpp"
#include "fileshare/v2/wire.hpp"

namespace fileshare::v2 {

namespace {

void send(net::Socket& s, const std::vector<std::uint8_t>& frame) {
    send_frame(s, frame);
}

void send_error(net::Socket& s, ErrCode code, const std::string& msg) {
    send(s, encode_error(ErrorMessage{code, msg}));
}

std::string ip_of(const std::string& peer) {
    const auto pos = peer.rfind(':');
    return pos == std::string::npos ? peer : peer.substr(0, pos);
}

// --- Download streaming -----------------------------------------------------
void handle_download(net::Socket& sock, ServerContext& ctx, Session& session, const Frame& fr) {
    const DownloadRequest req = parse_download_request(fr.payload.data(), fr.payload.size());
    const std::string norm = normalize_vpath(req.path);

    // Friendly directory error (metadata only). The streamed CONTENT comes from
    // open_beneath() below, which opens confined beneath the share root
    // (openat2 RESOLVE_BENEATH on Linux), so a concurrent symlink swap of an
    // intermediate component cannot make us stream a file outside the root.
    {
        std::error_code ec;
        if (std::filesystem::is_directory(ctx.vfs().resolve(norm), ec)) {
            throw FsError(ErrCode::IS_A_DIRECTORY, "cannot download a directory");
        }
    }
    std::unique_ptr<std::istream> in_ptr = ctx.vfs().open_beneath(norm);   // throws FsError
    std::istream& in = *in_ptr;

    in.seekg(0, std::ios::end);
    const std::streamoff endpos = in.tellg();
    if (endpos < 0) {
        throw FsError(ErrCode::INTERNAL_ERROR, "cannot size: " + norm);
    }
    const std::uint64_t total = static_cast<std::uint64_t>(endpos);
    if (req.offset > total) {
        send_error(sock, ErrCode::UNSUPPORTED_OFFSET, "offset past end of file");
        return;
    }
    in.seekg(static_cast<std::streamoff>(req.offset));

    const std::uint32_t tid = ctx.next_transfer_id();
    // Mark the transfer in-flight BEFORE announcing it and keep it marked until
    // DOWNLOAD_DONE has been sent, so graceful drain never misreads an active
    // transfer as idle (and never tears it down mid-stream).
    session.set_current_path(norm);
    send(sock, encode_download_accept(DownloadAccept{tid, total}));

    std::vector<char> buf(CHUNK_SIZE);
    std::uint64_t remaining = total - req.offset;
    bool failed = false;
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(CHUNK_SIZE, remaining));
        in.read(buf.data(), static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            failed = true;
            break;
        }
        const auto n = static_cast<std::size_t>(got);
        send(sock, encode_chunk_data(tid, reinterpret_cast<const std::uint8_t*>(buf.data()), n));
        remaining -= n;
        ctx.add_bytes(n);
        session.add_bytes(n);
    }

    if (failed) {
        session.set_current_path("");
        send_error(sock, ErrCode::INTERNAL_ERROR, "read error during transfer");
        return;
    }

    // Full-file checksum (cached) -- independent of the resume offset. Computed
    // while still marked in-flight so drain waits for it before tearing down.
    const ChecksumResponse cr = ctx.vfs().checksum(norm);
    DownloadDone done;
    done.transfer_id = tid;
    done.algo = cr.algo;
    done.checksum = cr.checksum;
    send(sock, encode_download_done(done));
    session.set_current_path("");
    ctx.inc_completed();
}

// --- Admin handlers (M7 subset: read + kick + shutdown) ---------------------
void handle_admin(net::Socket& sock, ServerContext& ctx, Session& session, const Frame& fr) {
    switch (fr.type) {
        case Msg::ADMIN_GET_CONFIG:
            send(sock, encode_admin_config(ctx.settings().to_json_string()));
            break;
        case Msg::ADMIN_STATS:
            send(sock, encode_admin_stats_response(ctx.stats_snapshot()));
            break;
        case Msg::ADMIN_LIST_CLIENTS: {
            std::vector<AdminClientInfo> out;
            for (const auto& s : ctx.sessions().snapshot()) {
                AdminClientInfo c;
                c.session_id = s.id;
                c.login = s.login;
                c.ip = s.ip;
                c.role = s.role;
                c.current_path = s.current_path;
                c.bytes_sent = s.bytes_sent;
                c.speed_bps = s.speed_bps;
                out.push_back(std::move(c));
            }
            send(sock, encode_admin_clients(out));
            break;
        }
        case Msg::ADMIN_KICK: {
            const AdminKick k = parse_admin_kick(fr.payload.data(), fr.payload.size());
            AdminKickResult res;
            if (k.session_id == session.id()) {
                res.ok = false;
                res.message = "refusing to kick yourself";
            } else {
                res.ok = ctx.sessions().kick(k.session_id);
                res.message = res.ok ? "kicked" : "no such session";
            }
            log_info("admin '" + session.login() + "' kick session " +
                     std::to_string(k.session_id) + ": " + res.message);
            send(sock, encode_admin_kick_result(res));
            break;
        }
        case Msg::ADMIN_SHUTDOWN: {
            const AdminShutdown sd = parse_admin_shutdown(fr.payload.data(), fr.payload.size());
            log_warn("admin '" + session.login() + "' requested shutdown (grace " +
                     std::to_string(sd.grace_seconds) + "s)");
            send(sock, encode_admin_shutdown_result(AdminShutdownResult{true, "shutting down"}));
            ctx.request_stop();
            break;
        }
        case Msg::ADMIN_SET:
            // Hot config apply lands in M11 (SettingsHub). Reject clearly for now.
            send(sock, encode_admin_set_result(
                     AdminSetResult{false, "live config changes not available yet (M11)"}));
            break;
        default:
            send_error(sock, ErrCode::BAD_REQUEST, "unexpected admin message");
            break;
    }
}

// --- One request ------------------------------------------------------------
void dispatch(net::Socket& sock, ServerContext& ctx, Session& session, const Frame& fr) {
    switch (fr.type) {
        case Msg::PING:
            send(sock, encode_pong());
            break;
        case Msg::PONG:
            break;   // heartbeat reply, nothing to do
        case Msg::SUBSCRIBE: {
            const Subscribe s = parse_subscribe(fr.payload.data(), fr.payload.size());
            session.set_subscription(s.mask);   // events delivered from M10
            break;
        }
        case Msg::LIST_DIR_REQUEST: {
            const ListDirRequest r = parse_list_dir_request(fr.payload.data(), fr.payload.size());
            ListDirResponse resp;
            resp.path = normalize_vpath(r.path);
            resp.entries = ctx.vfs().list(resp.path);
            send(sock, encode_list_dir_response(resp));
            break;
        }
        case Msg::STAT_REQUEST: {
            const StatRequest r = parse_stat_request(fr.payload.data(), fr.payload.size());
            StatResponse resp;
            resp.path = normalize_vpath(r.path);
            resp.entry = ctx.vfs().stat(resp.path);
            send(sock, encode_stat_response(resp));
            break;
        }
        case Msg::CHECKSUM_REQUEST: {
            const ChecksumRequest r = parse_checksum_request(fr.payload.data(), fr.payload.size());
            send(sock, encode_checksum_response(ctx.vfs().checksum(r.path)));
            break;
        }
        case Msg::DOWNLOAD_REQUEST:
            handle_download(sock, ctx, session, fr);
            break;
        case Msg::DOWNLOAD_CANCEL:
            // M7 runs one transfer to completion per request; nothing to cancel
            // between requests. Mid-stream cancel arrives with the epoll port.
            break;
        case Msg::ADMIN_GET_CONFIG:
        case Msg::ADMIN_STATS:
        case Msg::ADMIN_LIST_CLIENTS:
        case Msg::ADMIN_KICK:
        case Msg::ADMIN_SHUTDOWN:
        case Msg::ADMIN_SET:
            handle_admin(sock, ctx, session, fr);
            break;
        default:
            send_error(sock, ErrCode::BAD_REQUEST, "unexpected message type");
            break;
    }
}

// --- Handshake --------------------------------------------------------------
// Returns true if the session is authenticated and the request loop may run.
bool do_handshake(net::Socket& sock, ServerContext& ctx, Session& session) {
    const int hs_ms = static_cast<int>(ctx.settings().limits.handshake_timeout_s) * 1000;

    // Reject a banned IP before doing any work.
    if (ctx.auth_guard().banned(session.ip(), std::chrono::steady_clock::now())) {
        send_error(sock, ErrCode::RATE_LIMITED, "too many failed attempts; try again later");
        return false;
    }

    // 1. HELLO
    if (!net::wait_readable(sock, hs_ms)) {
        log_debug("session " + std::to_string(session.id()) + " handshake timeout (no HELLO)");
        return false;
    }
    std::optional<Frame> hello_fr;
    try {
        hello_fr = recv_frame(sock);
    } catch (const ProtocolError&) {
        // Likely a v1 client (its first byte is not a valid v2 type).
        send_error(sock, ErrCode::UNSUPPORTED_VERSION, "this server speaks protocol v2");
        return false;
    }
    if (!hello_fr) return false;
    if (hello_fr->type != Msg::HELLO) {
        send_error(sock, ErrCode::BAD_REQUEST, "expected HELLO");
        return false;
    }
    const Hello hello = parse_hello(hello_fr->payload.data(), hello_fr->payload.size());
    if (hello.proto_ver != PROTO_VERSION) {
        send_error(sock, ErrCode::UNSUPPORTED_VERSION, "unsupported protocol version");
        return false;
    }

    // 2. HELLO_OK -- advertise auth mode + challenge.
    const bool need_auth = ctx.auth_required();
    Challenge challenge{};
    HelloOk ok;
    ok.server_name = "fileshare-daemon";
    if (need_auth) {
        crypto::random_bytes(challenge.data(), challenge.size());
        ok.auth_mode = AUTH_MODE_CHALLENGE;
        ok.challenge = challenge;
        ok.pbkdf2_iters = ctx.pbkdf2_iters();
    } else {
        ok.auth_mode = AUTH_MODE_NONE;
    }
    send(sock, encode_hello_ok(ok));

    // 3. AUTH_REQUEST
    if (!net::wait_readable(sock, hs_ms)) {
        log_debug("session " + std::to_string(session.id()) + " handshake timeout (no AUTH)");
        return false;
    }
    std::optional<Frame> auth_fr;
    try {
        auth_fr = recv_frame(sock);
    } catch (const ProtocolError& e) {
        send_error(sock, ErrCode::BAD_REQUEST, e.what());
        return false;
    }
    if (!auth_fr) return false;
    if (auth_fr->type != Msg::AUTH_REQUEST) {
        send_error(sock, ErrCode::AUTH_REQUIRED, "expected AUTH_REQUEST");
        return false;
    }
    const AuthRequest areq = parse_auth_request(auth_fr->payload.data(), auth_fr->payload.size());

    std::string login;
    Role role = Role::USER;
    if (!need_auth) {
        // No-auth bootstrap: accept anyone, grant ADMIN so a fresh deployment is
        // usable before users are configured.
        login = areq.login.empty() ? "anonymous" : areq.login;
        role = Role::ADMIN;
    } else {
        const auto now = std::chrono::steady_clock::now();
        const std::optional<User> user = ctx.find_user(areq.login);
        const bool ok_auth =
            user.has_value() &&
            verify_client_proof(*user, challenge, areq.proof, ctx.pbkdf2_iters());
        if (!ok_auth) {
            ctx.auth_guard().fail(session.ip(), now, ctx.settings().limits.auth_fail_ban_s);
            // Slow down online guessing a touch (thread-per-connection: only this
            // attacker's thread pays).
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            send(sock, encode_auth_fail(AuthFail{7, "invalid login or password"}));
            log_warn("auth failed for '" + areq.login + "' from " + session.ip());
            return false;
        }
        // Enforce max concurrent sessions per user.
        const std::uint32_t max_sess = ctx.settings().limits.max_sessions_per_user;
        if (max_sess != 0 && ctx.sessions().sessions_for_login(user->login) >= max_sess) {
            send(sock, encode_auth_fail(AuthFail{8, "too many concurrent sessions"}));
            log_warn("session cap reached for '" + user->login + "'");
            return false;
        }
        ctx.auth_guard().succeed(session.ip());
        login = user->login;
        role = user->role;
    }

    session.authenticate(login, role);
    AuthOk aok;
    aok.role = role;
    aok.session_id = session.id();
    aok.motd = ctx.settings().motd;
    send(sock, encode_auth_ok(aok));
    log_info("session " + std::to_string(session.id()) + " authed as '" + login +
             "' (" + role_to_string(role) + ") from " + session.ip());
    return true;
}

} // namespace

// --- Connection handler -----------------------------------------------------
void handle_connection(net::Socket sock, ServerContext& ctx, std::string peer) {
    // Balances ctx.handler_started() (called by the accept thread before detach);
    // guarantees the count drops even on an exception, so wait_for_handlers()
    // in serve() cannot return while this thread is still touching ctx.
    struct HandlerGuard {
        ServerContext& c;
        ~HandlerGuard() { c.handler_finished(); }
    } guard{ctx};

    auto session = ctx.sessions().add(ip_of(peer), sock.handle());
    const std::uint64_t sid = session->id();

    try {
        if (do_handshake(sock, ctx, *session)) {
            const int idle_ms = static_cast<int>(ctx.settings().limits.idle_timeout_s) * 1000;
            while (ctx.accepting()) {
                if (!net::wait_readable(sock, idle_ms)) {
                    log_debug("session " + std::to_string(sid) + " idle timeout");
                    break;
                }
                std::optional<Frame> fr;
                try {
                    fr = recv_frame(sock);
                } catch (const ProtocolError& e) {
                    send_error(sock, ErrCode::BAD_REQUEST, e.what());
                    break;   // malformed framing drops only this connection
                }
                if (!fr) break;   // clean close

                if (!role_allows(session->role(), min_role(fr->type))) {
                    send_error(sock, ErrCode::ACCESS_DENIED, "insufficient role");
                    continue;
                }
                try {
                    dispatch(sock, ctx, *session, *fr);
                } catch (const FsError& e) {
                    send_error(sock, e.code(), e.what());     // recoverable, keep going
                } catch (const ProtocolError& e) {
                    send_error(sock, ErrCode::BAD_REQUEST, e.what());
                    break;    // malformed payload drops the connection
                }
            }
        }
    } catch (const net::NetError&) {
        // peer vanished mid-exchange -- just clean up
    } catch (const std::exception& e) {
        log_warn("session " + std::to_string(sid) + " error: " + e.what());
    }

    ctx.sessions().remove(sid);
    sock.close();
}

// --- Server -----------------------------------------------------------------
std::uint16_t Server::bind(std::uint16_t port) {
    std::uint16_t bound = 0;
    listener_ = net::tcp_listen(port, &bound, 64);
    return bound;
}

void Server::serve(std::uint32_t grace_seconds) {
    ctx_.mark_started();
    net::set_nonblocking(listener_, true);
    log_info("serving on port -- v2 daemon ready");

    while (ctx_.accepting()) {
        if (!net::wait_readable(listener_, 200)) {
            continue;
        }
        std::string peer;
        std::optional<net::Socket> s;
        try {
            s = net::tcp_accept(listener_, &peer);
        } catch (const net::NetError& e) {
            log_warn(std::string("accept failed: ") + e.what());
            continue;
        }
        if (!s) continue;   // half-open aborted / would-block

        const std::uint64_t max_conn = ctx_.settings().limits.max_connections;
        if (max_conn != 0 && static_cast<std::uint64_t>(ctx_.active_handlers()) >= max_conn) {
            // A broken pipe here must not unwind out of serve() and kill the
            // daemon -- rejecting an over-capacity client is best-effort.
            try {
                send_error(*s, ErrCode::RATE_LIMITED, "server at capacity");
            } catch (const net::NetError&) {
                // client already gone; nothing to do
            }
            s->close();
            continue;
        }
        // Count the handler live BEFORE detaching, so the drain barrier below
        // can never observe a not-yet-started thread as "done".
        ctx_.handler_started();
        std::thread(handle_connection, std::move(*s), std::ref(ctx_), peer).detach();
    }

    // --- Graceful drain -----------------------------------------------------
    log_info("draining (grace " + std::to_string(grace_seconds) + "s)");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(grace_seconds);
    ctx_.sessions().shutdown_idle();   // free connections that aren't downloading
    while (ctx_.sessions().downloading_count() > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ctx_.sessions().shutdown_all();    // force-close whatever remains (unblocks recv/send)
    // Wait unconditionally for every handler thread to return before we let the
    // caller destroy the ServerContext. shutdown_all() has closed all sockets,
    // so blocked threads wake immediately; a thread mid-checksum finishes and
    // exits. This closes the detached-thread use-after-free window.
    ctx_.wait_for_handlers();
    ctx_.save_cache();
    log_info("stopped");
}

} // namespace fileshare::v2
