#pragma once

// Protocol v2 (see docs/v2/02-protocol-v2.md).
//
// Reuses the v1 wire primitives (write_u*/read_u*/ByteReader/ProtocolError from
// fileshare/protocol.hpp) but defines its own message-type table, size limits
// and message set. v1 and v2 share framing (5-byte header + payload, big-endian)
// but are otherwise independent: a v2 server rejects a v1 client's first frame
// with ERROR(UNSUPPORTED_VERSION) and vice versa.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fileshare/protocol.hpp"  // write_u*/read_u*/ByteReader/ProtocolError
#include "fileshare/types.hpp"     // Checksum, CHECKSUM_LEN

namespace fileshare::v2 {

// --- Constants --------------------------------------------------------------
inline constexpr std::uint16_t PROTO_VERSION        = 2;
inline constexpr std::size_t   MAX_PATH_LEN         = 4096;        // bytes
inline constexpr std::size_t   MAX_NAME_LEN         = 255;         // single component
inline constexpr std::size_t   MAX_STRING_LEN       = 64 * 1024;   // generic u16-length string ceiling
inline constexpr std::uint32_t MAX_CONTROL_PAYLOAD  = 4u << 20;    // 4 MiB (big dir listings)
inline constexpr std::size_t   CHALLENGE_LEN        = 16;
inline constexpr std::size_t   PROOF_LEN            = 32;
inline constexpr std::uint32_t MAX_LIST_ENTRIES     = 1u << 20;    // 1M entries hard ceiling

using Challenge = std::array<std::uint8_t, CHALLENGE_LEN>;
using Proof     = std::array<std::uint8_t, PROOF_LEN>;

// --- Message types ----------------------------------------------------------
// Codes are grouped by concern; v1 codes 0x01-0x05 are deliberately NOT reused.
enum class Msg : std::uint8_t {
    // Service (shared shape with v1)
    ERROR_MSG            = 0x06,
    PING                 = 0x07,
    PONG                 = 0x08,
    // Handshake / auth
    HELLO                = 0x10,
    HELLO_OK             = 0x11,
    AUTH_REQUEST         = 0x12,
    AUTH_OK              = 0x13,
    AUTH_FAIL            = 0x14,
    // Filesystem
    LIST_DIR_REQUEST     = 0x20,
    LIST_DIR_RESPONSE    = 0x21,
    STAT_REQUEST         = 0x22,
    STAT_RESPONSE        = 0x23,
    CHECKSUM_REQUEST     = 0x24,
    CHECKSUM_RESPONSE    = 0x25,
    // Transfer
    DOWNLOAD_REQUEST     = 0x30,
    DOWNLOAD_ACCEPT      = 0x31,
    CHUNK_DATA           = 0x32,
    DOWNLOAD_DONE        = 0x33,
    DOWNLOAD_CANCEL      = 0x34,
    // Events (server push)
    SUBSCRIBE            = 0x40,
    EVENT_FS             = 0x41,
    EVENT_NOTICE         = 0x42,
    EVENT_CONFIG         = 0x43,
    // Admin
    ADMIN_GET_CONFIG     = 0x50,
    ADMIN_CONFIG         = 0x51,
    ADMIN_SET            = 0x52,
    ADMIN_SET_RESULT     = 0x53,
    ADMIN_LIST_CLIENTS   = 0x54,
    ADMIN_CLIENTS        = 0x55,
    ADMIN_KICK           = 0x56,
    ADMIN_KICK_RESULT    = 0x57,
    ADMIN_STATS          = 0x58,
    ADMIN_STATS_RESPONSE = 0x59,
    ADMIN_SHUTDOWN       = 0x5A,
    ADMIN_SHUTDOWN_RESULT= 0x5B,
};

[[nodiscard]] bool is_known_message_type(std::uint8_t raw) noexcept;
[[nodiscard]] const char* msg_name(Msg m) noexcept;

// --- Error / role / kind enums ---------------------------------------------
enum class ErrCode : std::uint16_t {
    OK                   = 0,
    FILE_NOT_FOUND       = 1,
    UNSUPPORTED_OFFSET   = 2,
    BAD_REQUEST          = 3,
    INTERNAL_ERROR       = 4,
    UNSUPPORTED_VERSION  = 5,
    AUTH_REQUIRED        = 6,
    AUTH_FAILED          = 7,
    ACCESS_DENIED        = 8,
    NOT_A_DIRECTORY      = 9,
    IS_A_DIRECTORY       = 10,
    RATE_LIMITED         = 11,
    SERVER_SHUTTING_DOWN = 12,
    QUOTA_EXCEEDED       = 13,
};

enum class Role : std::uint8_t { ANONYMOUS = 0, USER = 1, ADMIN = 2 };
enum class EntryKind : std::uint8_t { FILE = 0, DIR = 1 };
enum class FsOp : std::uint8_t { CREATED = 1, MODIFIED = 2, REMOVED = 3 };
enum class Severity : std::uint8_t { INFO = 0, WARN = 1, ERROR = 2 };

// auth_mode field of HELLO_OK
inline constexpr std::uint8_t AUTH_MODE_NONE      = 0;
inline constexpr std::uint8_t AUTH_MODE_CHALLENGE = 1;

// algo field of CHECKSUM_RESPONSE / DOWNLOAD_DONE
inline constexpr std::uint8_t ALGO_PENDING = 0;   // still being computed
inline constexpr std::uint8_t ALGO_CRC32   = 1;
inline constexpr std::uint8_t ALGO_SHA256  = 2;

// SUBSCRIBE mask bits
inline constexpr std::uint32_t SUB_FS      = 1u << 0;
inline constexpr std::uint32_t SUB_NOTICE  = 1u << 1;
inline constexpr std::uint32_t SUB_CONFIG  = 1u << 2;

// DirEntry flag bits
inline constexpr std::uint8_t ENTRY_FLAG_NEW = 1u << 0;

// --- Message payload structs ------------------------------------------------
struct Hello        { std::uint16_t proto_ver = PROTO_VERSION; std::string client_name; };
struct HelloOk      { std::uint16_t proto_ver = PROTO_VERSION; std::string server_name;
                      std::uint8_t auth_mode = AUTH_MODE_NONE; Challenge challenge{};
                      std::uint32_t pbkdf2_iters = 0; };  // KDF cost for challenge mode
struct AuthRequest  { std::string login; Proof proof{}; };
struct AuthOk       { Role role = Role::USER; std::uint64_t session_id = 0; std::string motd; };
struct AuthFail     { std::uint16_t reason = 0; std::string message; };

struct DirEntry {
    std::string   name;              // single path component (basename)
    EntryKind     kind = EntryKind::FILE;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;         // unix seconds
    std::uint8_t  flags = 0;
};
struct ListDirRequest  { std::string path; };
struct ListDirResponse { std::string path; std::vector<DirEntry> entries; };
struct StatRequest     { std::string path; };
struct StatResponse    { std::string path; DirEntry entry; };
struct ChecksumRequest { std::string path; };
struct ChecksumResponse{ std::string path; std::uint8_t algo = ALGO_PENDING; Checksum checksum{}; };

struct DownloadRequest { std::string path; std::uint64_t offset = 0; };
struct DownloadAccept  { std::uint32_t transfer_id = 0; std::uint64_t total_size = 0; };
struct DownloadDone    { std::uint32_t transfer_id = 0; std::uint8_t algo = ALGO_CRC32; Checksum checksum{}; };
struct DownloadCancel  { std::uint32_t transfer_id = 0; };

struct Subscribe    { std::uint32_t mask = 0; };
struct EventFs      { FsOp op = FsOp::CREATED; EntryKind kind = EntryKind::FILE;
                      std::string path; std::uint64_t size = 0; std::uint64_t mtime = 0; };
struct EventNotice  { Severity severity = Severity::INFO; std::string text; };
struct EventConfig  { std::string key; std::string new_value; };

struct ErrorMessage { ErrCode code = ErrCode::INTERNAL_ERROR; std::string message; };

// Admin payloads
struct AdminSet         { std::string key; std::string value; };
struct AdminSetResult   { bool ok = false; std::string message; };
struct AdminClientInfo  { std::uint64_t session_id = 0; std::string login; std::string ip;
                          Role role = Role::USER; std::string current_path;
                          std::uint64_t bytes_sent = 0; std::uint64_t speed_bps = 0; };
struct AdminKick        { std::uint64_t session_id = 0; };
struct AdminKickResult  { bool ok = false; std::string message; };
struct AdminStats {
    std::uint64_t uptime_seconds = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t completed_downloads = 0;
    std::uint64_t active_connections = 0;
    std::uint64_t active_downloads = 0;
    std::uint64_t shared_files = 0;
    std::uint64_t per_client_bps = 0;
    std::uint64_t global_bps = 0;
    std::string   version;
};
struct AdminShutdown       { std::uint32_t grace_seconds = 0; };
struct AdminShutdownResult { bool ok = false; std::string message; };

// --- Frame decode -----------------------------------------------------------
struct Frame { Msg type{}; std::vector<std::uint8_t> payload; };

// Parse the 5-byte v2 header. Throws ProtocolError on unknown type or
// payload_len > MAX_CONTROL_PAYLOAD.
struct FrameHeaderV2 { Msg type{}; std::uint32_t payload_len = 0; };
[[nodiscard]] FrameHeaderV2 parse_header(const std::uint8_t* p);

// Build a complete frame (header + payload). Throws on oversize payload.
[[nodiscard]] std::vector<std::uint8_t> make_frame(Msg type, const std::vector<std::uint8_t>& payload);

// Decode exactly one complete self-contained frame (header + declared payload).
[[nodiscard]] Frame decode_frame(const std::vector<std::uint8_t>& bytes);

// --- Encoders (each returns a complete frame) ------------------------------
[[nodiscard]] std::vector<std::uint8_t> encode_hello(const Hello&);
[[nodiscard]] std::vector<std::uint8_t> encode_hello_ok(const HelloOk&);
[[nodiscard]] std::vector<std::uint8_t> encode_auth_request(const AuthRequest&);
[[nodiscard]] std::vector<std::uint8_t> encode_auth_ok(const AuthOk&);
[[nodiscard]] std::vector<std::uint8_t> encode_auth_fail(const AuthFail&);

[[nodiscard]] std::vector<std::uint8_t> encode_list_dir_request(const ListDirRequest&);
[[nodiscard]] std::vector<std::uint8_t> encode_list_dir_response(const ListDirResponse&);
[[nodiscard]] std::vector<std::uint8_t> encode_stat_request(const StatRequest&);
[[nodiscard]] std::vector<std::uint8_t> encode_stat_response(const StatResponse&);
[[nodiscard]] std::vector<std::uint8_t> encode_checksum_request(const ChecksumRequest&);
[[nodiscard]] std::vector<std::uint8_t> encode_checksum_response(const ChecksumResponse&);

[[nodiscard]] std::vector<std::uint8_t> encode_download_request(const DownloadRequest&);
[[nodiscard]] std::vector<std::uint8_t> encode_download_accept(const DownloadAccept&);
[[nodiscard]] std::vector<std::uint8_t> encode_chunk_data(std::uint32_t transfer_id,
                                                          const std::uint8_t* data, std::size_t len);
[[nodiscard]] std::vector<std::uint8_t> encode_download_done(const DownloadDone&);
[[nodiscard]] std::vector<std::uint8_t> encode_download_cancel(const DownloadCancel&);

[[nodiscard]] std::vector<std::uint8_t> encode_subscribe(const Subscribe&);
[[nodiscard]] std::vector<std::uint8_t> encode_event_fs(const EventFs&);
[[nodiscard]] std::vector<std::uint8_t> encode_event_notice(const EventNotice&);
[[nodiscard]] std::vector<std::uint8_t> encode_event_config(const EventConfig&);

[[nodiscard]] std::vector<std::uint8_t> encode_error(const ErrorMessage&);
[[nodiscard]] std::vector<std::uint8_t> encode_ping();
[[nodiscard]] std::vector<std::uint8_t> encode_pong();

[[nodiscard]] std::vector<std::uint8_t> encode_admin_get_config();
[[nodiscard]] std::vector<std::uint8_t> encode_admin_config(const std::string& json);
[[nodiscard]] std::vector<std::uint8_t> encode_admin_set(const AdminSet&);
[[nodiscard]] std::vector<std::uint8_t> encode_admin_set_result(const AdminSetResult&);
[[nodiscard]] std::vector<std::uint8_t> encode_admin_list_clients();
[[nodiscard]] std::vector<std::uint8_t> encode_admin_clients(const std::vector<AdminClientInfo>&);
[[nodiscard]] std::vector<std::uint8_t> encode_admin_kick(const AdminKick&);
[[nodiscard]] std::vector<std::uint8_t> encode_admin_kick_result(const AdminKickResult&);
[[nodiscard]] std::vector<std::uint8_t> encode_admin_stats();
[[nodiscard]] std::vector<std::uint8_t> encode_admin_stats_response(const AdminStats&);
[[nodiscard]] std::vector<std::uint8_t> encode_admin_shutdown(const AdminShutdown&);
[[nodiscard]] std::vector<std::uint8_t> encode_admin_shutdown_result(const AdminShutdownResult&);

// --- Parsers (operate on a payload, no header). Throw ProtocolError. --------
[[nodiscard]] Hello           parse_hello(const std::uint8_t* p, std::size_t n);
[[nodiscard]] HelloOk         parse_hello_ok(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AuthRequest     parse_auth_request(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AuthOk          parse_auth_ok(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AuthFail        parse_auth_fail(const std::uint8_t* p, std::size_t n);

[[nodiscard]] ListDirRequest  parse_list_dir_request(const std::uint8_t* p, std::size_t n);
[[nodiscard]] ListDirResponse parse_list_dir_response(const std::uint8_t* p, std::size_t n);
[[nodiscard]] StatRequest     parse_stat_request(const std::uint8_t* p, std::size_t n);
[[nodiscard]] StatResponse    parse_stat_response(const std::uint8_t* p, std::size_t n);
[[nodiscard]] ChecksumRequest parse_checksum_request(const std::uint8_t* p, std::size_t n);
[[nodiscard]] ChecksumResponse parse_checksum_response(const std::uint8_t* p, std::size_t n);

[[nodiscard]] DownloadRequest parse_download_request(const std::uint8_t* p, std::size_t n);
[[nodiscard]] DownloadAccept  parse_download_accept(const std::uint8_t* p, std::size_t n);
// CHUNK_DATA: returns transfer_id and a pointer/len window into the payload.
struct ChunkView { std::uint32_t transfer_id = 0; const std::uint8_t* data = nullptr; std::size_t len = 0; };
[[nodiscard]] ChunkView       parse_chunk_data(const std::uint8_t* p, std::size_t n);
[[nodiscard]] DownloadDone    parse_download_done(const std::uint8_t* p, std::size_t n);
[[nodiscard]] DownloadCancel  parse_download_cancel(const std::uint8_t* p, std::size_t n);

[[nodiscard]] Subscribe       parse_subscribe(const std::uint8_t* p, std::size_t n);
[[nodiscard]] EventFs         parse_event_fs(const std::uint8_t* p, std::size_t n);
[[nodiscard]] EventNotice     parse_event_notice(const std::uint8_t* p, std::size_t n);
[[nodiscard]] EventConfig     parse_event_config(const std::uint8_t* p, std::size_t n);

[[nodiscard]] ErrorMessage    parse_error(const std::uint8_t* p, std::size_t n);

[[nodiscard]] std::string          parse_admin_config(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AdminSet             parse_admin_set(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AdminSetResult       parse_admin_set_result(const std::uint8_t* p, std::size_t n);
[[nodiscard]] std::vector<AdminClientInfo> parse_admin_clients(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AdminKick            parse_admin_kick(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AdminKickResult      parse_admin_kick_result(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AdminStats           parse_admin_stats_response(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AdminShutdown        parse_admin_shutdown(const std::uint8_t* p, std::size_t n);
[[nodiscard]] AdminShutdownResult  parse_admin_shutdown_result(const std::uint8_t* p, std::size_t n);

} // namespace fileshare::v2
