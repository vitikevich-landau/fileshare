#include "fileshare/v2/protocol.hpp"

#include <cstring>
#include <utility>

namespace fileshare::v2 {

// --- Message-type table -----------------------------------------------------
bool is_known_message_type(std::uint8_t raw) noexcept {
    switch (static_cast<Msg>(raw)) {
        case Msg::ERROR_MSG:
        case Msg::PING:
        case Msg::PONG:
        case Msg::HELLO:
        case Msg::HELLO_OK:
        case Msg::AUTH_REQUEST:
        case Msg::AUTH_OK:
        case Msg::AUTH_FAIL:
        case Msg::LIST_DIR_REQUEST:
        case Msg::LIST_DIR_RESPONSE:
        case Msg::STAT_REQUEST:
        case Msg::STAT_RESPONSE:
        case Msg::CHECKSUM_REQUEST:
        case Msg::CHECKSUM_RESPONSE:
        case Msg::DOWNLOAD_REQUEST:
        case Msg::DOWNLOAD_ACCEPT:
        case Msg::CHUNK_DATA:
        case Msg::DOWNLOAD_DONE:
        case Msg::DOWNLOAD_CANCEL:
        case Msg::SUBSCRIBE:
        case Msg::EVENT_FS:
        case Msg::EVENT_NOTICE:
        case Msg::EVENT_CONFIG:
        case Msg::ADMIN_GET_CONFIG:
        case Msg::ADMIN_CONFIG:
        case Msg::ADMIN_SET:
        case Msg::ADMIN_SET_RESULT:
        case Msg::ADMIN_LIST_CLIENTS:
        case Msg::ADMIN_CLIENTS:
        case Msg::ADMIN_KICK:
        case Msg::ADMIN_KICK_RESULT:
        case Msg::ADMIN_STATS:
        case Msg::ADMIN_STATS_RESPONSE:
        case Msg::ADMIN_SHUTDOWN:
        case Msg::ADMIN_SHUTDOWN_RESULT:
            return true;
    }
    return false;
}

const char* msg_name(Msg m) noexcept {
    switch (m) {
        case Msg::ERROR_MSG:            return "ERROR";
        case Msg::PING:                 return "PING";
        case Msg::PONG:                 return "PONG";
        case Msg::HELLO:                return "HELLO";
        case Msg::HELLO_OK:             return "HELLO_OK";
        case Msg::AUTH_REQUEST:         return "AUTH_REQUEST";
        case Msg::AUTH_OK:              return "AUTH_OK";
        case Msg::AUTH_FAIL:            return "AUTH_FAIL";
        case Msg::LIST_DIR_REQUEST:     return "LIST_DIR_REQUEST";
        case Msg::LIST_DIR_RESPONSE:    return "LIST_DIR_RESPONSE";
        case Msg::STAT_REQUEST:         return "STAT_REQUEST";
        case Msg::STAT_RESPONSE:        return "STAT_RESPONSE";
        case Msg::CHECKSUM_REQUEST:     return "CHECKSUM_REQUEST";
        case Msg::CHECKSUM_RESPONSE:    return "CHECKSUM_RESPONSE";
        case Msg::DOWNLOAD_REQUEST:     return "DOWNLOAD_REQUEST";
        case Msg::DOWNLOAD_ACCEPT:      return "DOWNLOAD_ACCEPT";
        case Msg::CHUNK_DATA:           return "CHUNK_DATA";
        case Msg::DOWNLOAD_DONE:        return "DOWNLOAD_DONE";
        case Msg::DOWNLOAD_CANCEL:      return "DOWNLOAD_CANCEL";
        case Msg::SUBSCRIBE:            return "SUBSCRIBE";
        case Msg::EVENT_FS:             return "EVENT_FS";
        case Msg::EVENT_NOTICE:         return "EVENT_NOTICE";
        case Msg::EVENT_CONFIG:         return "EVENT_CONFIG";
        case Msg::ADMIN_GET_CONFIG:     return "ADMIN_GET_CONFIG";
        case Msg::ADMIN_CONFIG:         return "ADMIN_CONFIG";
        case Msg::ADMIN_SET:            return "ADMIN_SET";
        case Msg::ADMIN_SET_RESULT:     return "ADMIN_SET_RESULT";
        case Msg::ADMIN_LIST_CLIENTS:   return "ADMIN_LIST_CLIENTS";
        case Msg::ADMIN_CLIENTS:        return "ADMIN_CLIENTS";
        case Msg::ADMIN_KICK:           return "ADMIN_KICK";
        case Msg::ADMIN_KICK_RESULT:    return "ADMIN_KICK_RESULT";
        case Msg::ADMIN_STATS:          return "ADMIN_STATS";
        case Msg::ADMIN_STATS_RESPONSE: return "ADMIN_STATS_RESPONSE";
        case Msg::ADMIN_SHUTDOWN:       return "ADMIN_SHUTDOWN";
        case Msg::ADMIN_SHUTDOWN_RESULT:return "ADMIN_SHUTDOWN_RESULT";
    }
    return "UNKNOWN";
}

// --- Local helpers ----------------------------------------------------------
namespace {

// A length-prefixed string: u16 length + UTF-8 bytes. `limit` guards both
// encode (throw on oversize) and is the caller's responsibility to pass.
void write_str(std::vector<std::uint8_t>& buf, const std::string& s, std::size_t limit) {
    if (s.size() > limit || s.size() > 0xFFFFu) {
        throw ProtocolError("string exceeds length limit");
    }
    write_u16be(buf, static_cast<std::uint16_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

std::string read_str(ByteReader& r, std::size_t limit) {
    const std::uint16_t len = r.u16be();
    if (len > limit) {
        throw ProtocolError("string exceeds length limit");
    }
    return r.str(len);
}

void write_checksum(std::vector<std::uint8_t>& buf, const Checksum& c) {
    buf.insert(buf.end(), c.begin(), c.end());
}

Checksum read_checksum(ByteReader& r) {
    Checksum c{};
    r.read_into(c.data(), c.size());
    return c;
}

void require_end(ByteReader& r, const char* what) {
    if (!r.empty()) {
        throw ProtocolError(std::string("trailing bytes in ") + what);
    }
}

} // namespace

// --- Frames -----------------------------------------------------------------
FrameHeaderV2 parse_header(const std::uint8_t* p) {
    const std::uint8_t raw = p[0];
    if (!is_known_message_type(raw)) {
        throw ProtocolError("unknown v2 message type");
    }
    const std::uint32_t len = read_u32be(p + 1);
    if (len > MAX_CONTROL_PAYLOAD) {
        throw ProtocolError("v2 payload length exceeds MAX_CONTROL_PAYLOAD");
    }
    return FrameHeaderV2{static_cast<Msg>(raw), len};
}

std::vector<std::uint8_t> make_frame(Msg type, const std::vector<std::uint8_t>& payload) {
    if (payload.size() > MAX_CONTROL_PAYLOAD) {
        throw ProtocolError("v2 frame payload exceeds MAX_CONTROL_PAYLOAD");
    }
    std::vector<std::uint8_t> out;
    out.reserve(HEADER_SIZE + payload.size());
    write_u8(out, static_cast<std::uint8_t>(type));
    write_u32be(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Frame decode_frame(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < HEADER_SIZE) {
        throw ProtocolError("v2 frame shorter than header");
    }
    const FrameHeaderV2 h = parse_header(bytes.data());
    if (bytes.size() - HEADER_SIZE != h.payload_len) {
        throw ProtocolError("v2 frame length does not match declared payload");
    }
    Frame f;
    f.type = h.type;
    f.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(HEADER_SIZE), bytes.end());
    return f;
}

// --- DirEntry helpers -------------------------------------------------------
namespace {
void write_dir_entry(std::vector<std::uint8_t>& p, const DirEntry& e) {
    write_str(p, e.name, MAX_NAME_LEN);
    write_u8(p, static_cast<std::uint8_t>(e.kind));
    write_u64be(p, e.size);
    write_u64be(p, e.mtime);
    write_u8(p, e.flags);
}

DirEntry read_dir_entry(ByteReader& r) {
    DirEntry e;
    e.name  = read_str(r, MAX_NAME_LEN);
    e.kind  = static_cast<EntryKind>(r.u8());
    e.size  = r.u64be();
    e.mtime = r.u64be();
    e.flags = r.u8();
    return e;
}
} // namespace

// --- Encoders: handshake / auth --------------------------------------------
std::vector<std::uint8_t> encode_hello(const Hello& h) {
    std::vector<std::uint8_t> p;
    write_u16be(p, h.proto_ver);
    write_str(p, h.client_name, MAX_NAME_LEN);
    return make_frame(Msg::HELLO, p);
}

std::vector<std::uint8_t> encode_hello_ok(const HelloOk& h) {
    std::vector<std::uint8_t> p;
    write_u16be(p, h.proto_ver);
    write_str(p, h.server_name, MAX_NAME_LEN);
    write_u8(p, h.auth_mode);
    p.insert(p.end(), h.challenge.begin(), h.challenge.end());
    write_u32be(p, h.pbkdf2_iters);
    return make_frame(Msg::HELLO_OK, p);
}

std::vector<std::uint8_t> encode_auth_request(const AuthRequest& a) {
    std::vector<std::uint8_t> p;
    write_str(p, a.login, MAX_NAME_LEN);
    p.insert(p.end(), a.proof.begin(), a.proof.end());
    return make_frame(Msg::AUTH_REQUEST, p);
}

std::vector<std::uint8_t> encode_auth_ok(const AuthOk& a) {
    std::vector<std::uint8_t> p;
    write_u8(p, static_cast<std::uint8_t>(a.role));
    write_u64be(p, a.session_id);
    write_str(p, a.motd, MAX_STRING_LEN);
    return make_frame(Msg::AUTH_OK, p);
}

std::vector<std::uint8_t> encode_auth_fail(const AuthFail& a) {
    std::vector<std::uint8_t> p;
    write_u16be(p, a.reason);
    write_str(p, a.message, MAX_STRING_LEN);
    return make_frame(Msg::AUTH_FAIL, p);
}

// --- Encoders: filesystem ---------------------------------------------------
std::vector<std::uint8_t> encode_list_dir_request(const ListDirRequest& r) {
    std::vector<std::uint8_t> p;
    write_str(p, r.path, MAX_PATH_LEN);
    return make_frame(Msg::LIST_DIR_REQUEST, p);
}

std::vector<std::uint8_t> encode_list_dir_response(const ListDirResponse& r) {
    std::vector<std::uint8_t> p;
    write_str(p, r.path, MAX_PATH_LEN);
    write_u32be(p, static_cast<std::uint32_t>(r.entries.size()));
    for (const auto& e : r.entries) {
        write_dir_entry(p, e);
    }
    return make_frame(Msg::LIST_DIR_RESPONSE, p);
}

std::vector<std::uint8_t> encode_stat_request(const StatRequest& r) {
    std::vector<std::uint8_t> p;
    write_str(p, r.path, MAX_PATH_LEN);
    return make_frame(Msg::STAT_REQUEST, p);
}

std::vector<std::uint8_t> encode_stat_response(const StatResponse& r) {
    std::vector<std::uint8_t> p;
    write_str(p, r.path, MAX_PATH_LEN);
    write_dir_entry(p, r.entry);
    return make_frame(Msg::STAT_RESPONSE, p);
}

std::vector<std::uint8_t> encode_checksum_request(const ChecksumRequest& r) {
    std::vector<std::uint8_t> p;
    write_str(p, r.path, MAX_PATH_LEN);
    return make_frame(Msg::CHECKSUM_REQUEST, p);
}

std::vector<std::uint8_t> encode_checksum_response(const ChecksumResponse& r) {
    std::vector<std::uint8_t> p;
    write_str(p, r.path, MAX_PATH_LEN);
    write_u8(p, r.algo);
    write_checksum(p, r.checksum);
    return make_frame(Msg::CHECKSUM_RESPONSE, p);
}

// --- Encoders: transfer -----------------------------------------------------
std::vector<std::uint8_t> encode_download_request(const DownloadRequest& r) {
    std::vector<std::uint8_t> p;
    write_str(p, r.path, MAX_PATH_LEN);
    write_u64be(p, r.offset);
    return make_frame(Msg::DOWNLOAD_REQUEST, p);
}

std::vector<std::uint8_t> encode_download_accept(const DownloadAccept& a) {
    std::vector<std::uint8_t> p;
    write_u32be(p, a.transfer_id);
    write_u64be(p, a.total_size);
    return make_frame(Msg::DOWNLOAD_ACCEPT, p);
}

std::vector<std::uint8_t> encode_chunk_data(std::uint32_t transfer_id,
                                            const std::uint8_t* data, std::size_t len) {
    std::vector<std::uint8_t> p;
    p.reserve(4 + len);
    write_u32be(p, transfer_id);
    p.insert(p.end(), data, data + len);
    return make_frame(Msg::CHUNK_DATA, p);
}

std::vector<std::uint8_t> encode_download_done(const DownloadDone& d) {
    std::vector<std::uint8_t> p;
    write_u32be(p, d.transfer_id);
    write_u8(p, d.algo);
    write_checksum(p, d.checksum);
    return make_frame(Msg::DOWNLOAD_DONE, p);
}

std::vector<std::uint8_t> encode_download_cancel(const DownloadCancel& d) {
    std::vector<std::uint8_t> p;
    write_u32be(p, d.transfer_id);
    return make_frame(Msg::DOWNLOAD_CANCEL, p);
}

// --- Encoders: events -------------------------------------------------------
std::vector<std::uint8_t> encode_subscribe(const Subscribe& s) {
    std::vector<std::uint8_t> p;
    write_u32be(p, s.mask);
    return make_frame(Msg::SUBSCRIBE, p);
}

std::vector<std::uint8_t> encode_event_fs(const EventFs& e) {
    std::vector<std::uint8_t> p;
    write_u8(p, static_cast<std::uint8_t>(e.op));
    write_u8(p, static_cast<std::uint8_t>(e.kind));
    write_str(p, e.path, MAX_PATH_LEN);
    write_u64be(p, e.size);
    write_u64be(p, e.mtime);
    return make_frame(Msg::EVENT_FS, p);
}

std::vector<std::uint8_t> encode_event_notice(const EventNotice& e) {
    std::vector<std::uint8_t> p;
    write_u8(p, static_cast<std::uint8_t>(e.severity));
    write_str(p, e.text, MAX_STRING_LEN);
    return make_frame(Msg::EVENT_NOTICE, p);
}

std::vector<std::uint8_t> encode_event_config(const EventConfig& e) {
    std::vector<std::uint8_t> p;
    write_str(p, e.key, MAX_NAME_LEN);
    write_str(p, e.new_value, MAX_STRING_LEN);
    return make_frame(Msg::EVENT_CONFIG, p);
}

// --- Encoders: service ------------------------------------------------------
std::vector<std::uint8_t> encode_error(const ErrorMessage& e) {
    std::vector<std::uint8_t> p;
    write_u16be(p, static_cast<std::uint16_t>(e.code));
    write_str(p, e.message, MAX_STRING_LEN);
    return make_frame(Msg::ERROR_MSG, p);
}

std::vector<std::uint8_t> encode_ping() { return make_frame(Msg::PING, {}); }
std::vector<std::uint8_t> encode_pong() { return make_frame(Msg::PONG, {}); }

// --- Encoders: admin --------------------------------------------------------
std::vector<std::uint8_t> encode_admin_get_config() { return make_frame(Msg::ADMIN_GET_CONFIG, {}); }

std::vector<std::uint8_t> encode_admin_config(const std::string& json) {
    std::vector<std::uint8_t> p;
    // JSON can be large; length-prefix as u32 to allow > 64 KiB configs.
    if (json.size() > MAX_CONTROL_PAYLOAD - 4) {
        throw ProtocolError("admin config JSON too large");
    }
    write_u32be(p, static_cast<std::uint32_t>(json.size()));
    p.insert(p.end(), json.begin(), json.end());
    return make_frame(Msg::ADMIN_CONFIG, p);
}

std::vector<std::uint8_t> encode_admin_set(const AdminSet& a) {
    std::vector<std::uint8_t> p;
    write_str(p, a.key, MAX_NAME_LEN);
    write_str(p, a.value, MAX_STRING_LEN);
    return make_frame(Msg::ADMIN_SET, p);
}

std::vector<std::uint8_t> encode_admin_set_result(const AdminSetResult& a) {
    std::vector<std::uint8_t> p;
    write_u8(p, a.ok ? 1u : 0u);
    write_str(p, a.message, MAX_STRING_LEN);
    return make_frame(Msg::ADMIN_SET_RESULT, p);
}

std::vector<std::uint8_t> encode_admin_list_clients() { return make_frame(Msg::ADMIN_LIST_CLIENTS, {}); }

std::vector<std::uint8_t> encode_admin_clients(const std::vector<AdminClientInfo>& cs) {
    std::vector<std::uint8_t> p;
    write_u32be(p, static_cast<std::uint32_t>(cs.size()));
    for (const auto& c : cs) {
        write_u64be(p, c.session_id);
        write_str(p, c.login, MAX_NAME_LEN);
        write_str(p, c.ip, MAX_NAME_LEN);
        write_u8(p, static_cast<std::uint8_t>(c.role));
        write_str(p, c.current_path, MAX_PATH_LEN);
        write_u64be(p, c.bytes_sent);
        write_u64be(p, c.speed_bps);
    }
    return make_frame(Msg::ADMIN_CLIENTS, p);
}

std::vector<std::uint8_t> encode_admin_kick(const AdminKick& a) {
    std::vector<std::uint8_t> p;
    write_u64be(p, a.session_id);
    return make_frame(Msg::ADMIN_KICK, p);
}

std::vector<std::uint8_t> encode_admin_kick_result(const AdminKickResult& a) {
    std::vector<std::uint8_t> p;
    write_u8(p, a.ok ? 1u : 0u);
    write_str(p, a.message, MAX_STRING_LEN);
    return make_frame(Msg::ADMIN_KICK_RESULT, p);
}

std::vector<std::uint8_t> encode_admin_stats() { return make_frame(Msg::ADMIN_STATS, {}); }

std::vector<std::uint8_t> encode_admin_stats_response(const AdminStats& s) {
    std::vector<std::uint8_t> p;
    write_u64be(p, s.uptime_seconds);
    write_u64be(p, s.bytes_sent);
    write_u64be(p, s.completed_downloads);
    write_u64be(p, s.active_connections);
    write_u64be(p, s.active_downloads);
    write_u64be(p, s.shared_files);
    write_u64be(p, s.per_client_bps);
    write_u64be(p, s.global_bps);
    write_str(p, s.version, MAX_NAME_LEN);
    return make_frame(Msg::ADMIN_STATS_RESPONSE, p);
}

std::vector<std::uint8_t> encode_admin_shutdown(const AdminShutdown& a) {
    std::vector<std::uint8_t> p;
    write_u32be(p, a.grace_seconds);
    return make_frame(Msg::ADMIN_SHUTDOWN, p);
}

std::vector<std::uint8_t> encode_admin_shutdown_result(const AdminShutdownResult& a) {
    std::vector<std::uint8_t> p;
    write_u8(p, a.ok ? 1u : 0u);
    write_str(p, a.message, MAX_STRING_LEN);
    return make_frame(Msg::ADMIN_SHUTDOWN_RESULT, p);
}

// --- Parsers: handshake / auth ---------------------------------------------
Hello parse_hello(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    Hello h;
    h.proto_ver   = r.u16be();
    h.client_name = read_str(r, MAX_NAME_LEN);
    require_end(r, "HELLO");
    return h;
}

HelloOk parse_hello_ok(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    HelloOk h;
    h.proto_ver   = r.u16be();
    h.server_name = read_str(r, MAX_NAME_LEN);
    h.auth_mode   = r.u8();
    r.read_into(h.challenge.data(), h.challenge.size());
    h.pbkdf2_iters = r.u32be();
    require_end(r, "HELLO_OK");
    return h;
}

AuthRequest parse_auth_request(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AuthRequest a;
    a.login = read_str(r, MAX_NAME_LEN);
    r.read_into(a.proof.data(), a.proof.size());
    require_end(r, "AUTH_REQUEST");
    return a;
}

AuthOk parse_auth_ok(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AuthOk a;
    a.role       = static_cast<Role>(r.u8());
    a.session_id = r.u64be();
    a.motd       = read_str(r, MAX_STRING_LEN);
    require_end(r, "AUTH_OK");
    return a;
}

AuthFail parse_auth_fail(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AuthFail a;
    a.reason  = r.u16be();
    a.message = read_str(r, MAX_STRING_LEN);
    require_end(r, "AUTH_FAIL");
    return a;
}

// --- Parsers: filesystem ----------------------------------------------------
ListDirRequest parse_list_dir_request(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    ListDirRequest req;
    req.path = read_str(r, MAX_PATH_LEN);
    require_end(r, "LIST_DIR_REQUEST");
    return req;
}

ListDirResponse parse_list_dir_response(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    ListDirResponse resp;
    resp.path = read_str(r, MAX_PATH_LEN);
    const std::uint32_t count = r.u32be();
    if (count > MAX_LIST_ENTRIES) {
        throw ProtocolError("LIST_DIR_RESPONSE entry count exceeds ceiling");
    }
    // No reserve(count): a hostile count would OOM; each read is bounded.
    for (std::uint32_t i = 0; i < count; ++i) {
        resp.entries.push_back(read_dir_entry(r));
    }
    require_end(r, "LIST_DIR_RESPONSE");
    return resp;
}

StatRequest parse_stat_request(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    StatRequest req;
    req.path = read_str(r, MAX_PATH_LEN);
    require_end(r, "STAT_REQUEST");
    return req;
}

StatResponse parse_stat_response(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    StatResponse resp;
    resp.path  = read_str(r, MAX_PATH_LEN);
    resp.entry = read_dir_entry(r);
    require_end(r, "STAT_RESPONSE");
    return resp;
}

ChecksumRequest parse_checksum_request(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    ChecksumRequest req;
    req.path = read_str(r, MAX_PATH_LEN);
    require_end(r, "CHECKSUM_REQUEST");
    return req;
}

ChecksumResponse parse_checksum_response(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    ChecksumResponse resp;
    resp.path     = read_str(r, MAX_PATH_LEN);
    resp.algo     = r.u8();
    resp.checksum = read_checksum(r);
    require_end(r, "CHECKSUM_RESPONSE");
    return resp;
}

// --- Parsers: transfer ------------------------------------------------------
DownloadRequest parse_download_request(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    DownloadRequest req;
    req.path   = read_str(r, MAX_PATH_LEN);
    req.offset = r.u64be();
    require_end(r, "DOWNLOAD_REQUEST");
    return req;
}

DownloadAccept parse_download_accept(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    DownloadAccept a;
    a.transfer_id = r.u32be();
    a.total_size  = r.u64be();
    require_end(r, "DOWNLOAD_ACCEPT");
    return a;
}

ChunkView parse_chunk_data(const std::uint8_t* p, std::size_t n) {
    if (n < 4) {
        throw ProtocolError("CHUNK_DATA shorter than transfer_id");
    }
    ChunkView v;
    v.transfer_id = read_u32be(p);
    v.data = p + 4;
    v.len  = n - 4;
    return v;
}

DownloadDone parse_download_done(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    DownloadDone d;
    d.transfer_id = r.u32be();
    d.algo        = r.u8();
    d.checksum    = read_checksum(r);
    require_end(r, "DOWNLOAD_DONE");
    return d;
}

DownloadCancel parse_download_cancel(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    DownloadCancel d;
    d.transfer_id = r.u32be();
    require_end(r, "DOWNLOAD_CANCEL");
    return d;
}

// --- Parsers: events --------------------------------------------------------
Subscribe parse_subscribe(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    Subscribe s;
    s.mask = r.u32be();
    require_end(r, "SUBSCRIBE");
    return s;
}

EventFs parse_event_fs(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    EventFs e;
    e.op    = static_cast<FsOp>(r.u8());
    e.kind  = static_cast<EntryKind>(r.u8());
    e.path  = read_str(r, MAX_PATH_LEN);
    e.size  = r.u64be();
    e.mtime = r.u64be();
    require_end(r, "EVENT_FS");
    return e;
}

EventNotice parse_event_notice(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    EventNotice e;
    e.severity = static_cast<Severity>(r.u8());
    e.text     = read_str(r, MAX_STRING_LEN);
    require_end(r, "EVENT_NOTICE");
    return e;
}

EventConfig parse_event_config(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    EventConfig e;
    e.key       = read_str(r, MAX_NAME_LEN);
    e.new_value = read_str(r, MAX_STRING_LEN);
    require_end(r, "EVENT_CONFIG");
    return e;
}

// --- Parsers: service -------------------------------------------------------
ErrorMessage parse_error(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    ErrorMessage e;
    e.code    = static_cast<ErrCode>(r.u16be());
    e.message = read_str(r, MAX_STRING_LEN);
    require_end(r, "ERROR");
    return e;
}

// --- Parsers: admin ---------------------------------------------------------
std::string parse_admin_config(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    const std::uint32_t len = r.u32be();
    std::string json = r.str(len);
    require_end(r, "ADMIN_CONFIG");
    return json;
}

AdminSet parse_admin_set(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AdminSet a;
    a.key   = read_str(r, MAX_NAME_LEN);
    a.value = read_str(r, MAX_STRING_LEN);
    require_end(r, "ADMIN_SET");
    return a;
}

AdminSetResult parse_admin_set_result(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AdminSetResult a;
    a.ok      = r.u8() != 0;
    a.message = read_str(r, MAX_STRING_LEN);
    require_end(r, "ADMIN_SET_RESULT");
    return a;
}

std::vector<AdminClientInfo> parse_admin_clients(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    const std::uint32_t count = r.u32be();
    if (count > MAX_LIST_ENTRIES) {
        throw ProtocolError("ADMIN_CLIENTS count exceeds ceiling");
    }
    std::vector<AdminClientInfo> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        AdminClientInfo c;
        c.session_id   = r.u64be();
        c.login        = read_str(r, MAX_NAME_LEN);
        c.ip           = read_str(r, MAX_NAME_LEN);
        c.role         = static_cast<Role>(r.u8());
        c.current_path = read_str(r, MAX_PATH_LEN);
        c.bytes_sent   = r.u64be();
        c.speed_bps    = r.u64be();
        out.push_back(std::move(c));
    }
    require_end(r, "ADMIN_CLIENTS");
    return out;
}

AdminKick parse_admin_kick(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AdminKick a;
    a.session_id = r.u64be();
    require_end(r, "ADMIN_KICK");
    return a;
}

AdminKickResult parse_admin_kick_result(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AdminKickResult a;
    a.ok      = r.u8() != 0;
    a.message = read_str(r, MAX_STRING_LEN);
    require_end(r, "ADMIN_KICK_RESULT");
    return a;
}

AdminStats parse_admin_stats_response(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AdminStats s;
    s.uptime_seconds      = r.u64be();
    s.bytes_sent          = r.u64be();
    s.completed_downloads = r.u64be();
    s.active_connections  = r.u64be();
    s.active_downloads    = r.u64be();
    s.shared_files        = r.u64be();
    s.per_client_bps      = r.u64be();
    s.global_bps          = r.u64be();
    s.version             = read_str(r, MAX_NAME_LEN);
    require_end(r, "ADMIN_STATS_RESPONSE");
    return s;
}

AdminShutdown parse_admin_shutdown(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AdminShutdown a;
    a.grace_seconds = r.u32be();
    require_end(r, "ADMIN_SHUTDOWN");
    return a;
}

AdminShutdownResult parse_admin_shutdown_result(const std::uint8_t* p, std::size_t n) {
    ByteReader r(p, n);
    AdminShutdownResult a;
    a.ok      = r.u8() != 0;
    a.message = read_str(r, MAX_STRING_LEN);
    require_end(r, "ADMIN_SHUTDOWN_RESULT");
    return a;
}

} // namespace fileshare::v2
