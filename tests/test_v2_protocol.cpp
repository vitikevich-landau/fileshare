#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "fileshare/protocol.hpp"      // ProtocolError
#include "fileshare/v2/protocol.hpp"

using namespace fileshare::v2;
using fileshare::ProtocolError;  // shared exception type
using fileshare::HEADER_SIZE;    // shared frame-header size

namespace {

// Round-trip a frame through decode_frame and return the payload window.
Frame roundtrip(const std::vector<std::uint8_t>& frame) {
    return decode_frame(frame);
}

} // namespace

// --- Handshake / auth -------------------------------------------------------
TEST(V2Protocol, HelloRoundTrip) {
    Hello h;
    h.proto_ver = 2;
    h.client_name = "commander/2.0";
    const Frame f = roundtrip(encode_hello(h));
    ASSERT_EQ(f.type, Msg::HELLO);
    const Hello out = parse_hello(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.proto_ver, 2u);
    EXPECT_EQ(out.client_name, "commander/2.0");
}

TEST(V2Protocol, HelloOkRoundTrip) {
    HelloOk h;
    h.server_name = "fileshare-daemon";
    h.auth_mode = AUTH_MODE_CHALLENGE;
    h.pbkdf2_iters = 200000;
    for (std::size_t i = 0; i < CHALLENGE_LEN; ++i) h.challenge[i] = static_cast<std::uint8_t>(i);
    const Frame f = roundtrip(encode_hello_ok(h));
    ASSERT_EQ(f.type, Msg::HELLO_OK);
    const HelloOk out = parse_hello_ok(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.server_name, "fileshare-daemon");
    EXPECT_EQ(out.auth_mode, AUTH_MODE_CHALLENGE);
    EXPECT_EQ(out.challenge, h.challenge);
    EXPECT_EQ(out.pbkdf2_iters, 200000u);
}

TEST(V2Protocol, AuthRequestRoundTrip) {
    AuthRequest a;
    a.login = "vit";
    a.proof.fill(0xC3);
    const Frame f = roundtrip(encode_auth_request(a));
    ASSERT_EQ(f.type, Msg::AUTH_REQUEST);
    const AuthRequest out = parse_auth_request(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.login, "vit");
    EXPECT_EQ(out.proof, a.proof);
}

TEST(V2Protocol, AuthOkRoundTrip) {
    AuthOk a;
    a.role = Role::ADMIN;
    a.session_id = 0xDEADBEEFull;
    a.motd = "welcome";
    const Frame f = roundtrip(encode_auth_ok(a));
    ASSERT_EQ(f.type, Msg::AUTH_OK);
    const AuthOk out = parse_auth_ok(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.role, Role::ADMIN);
    EXPECT_EQ(out.session_id, 0xDEADBEEFull);
    EXPECT_EQ(out.motd, "welcome");
}

TEST(V2Protocol, AuthFailRoundTrip) {
    AuthFail a;
    a.reason = 7;
    a.message = "bad password";
    const Frame f = roundtrip(encode_auth_fail(a));
    const AuthFail out = parse_auth_fail(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.reason, 7u);
    EXPECT_EQ(out.message, "bad password");
}

// --- Filesystem -------------------------------------------------------------
TEST(V2Protocol, ListDirRoundTrip) {
    ListDirResponse r;
    r.path = "/incoming";
    DirEntry d1{"video", EntryKind::DIR, 0, 1000, 0};
    DirEntry d2{"new-build.tar.gz", EntryKind::FILE, 340u * 1024 * 1024, 2000, ENTRY_FLAG_NEW};
    r.entries = {d1, d2};
    const Frame f = roundtrip(encode_list_dir_response(r));
    ASSERT_EQ(f.type, Msg::LIST_DIR_RESPONSE);
    const ListDirResponse out = parse_list_dir_response(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.path, "/incoming");
    ASSERT_EQ(out.entries.size(), 2u);
    EXPECT_EQ(out.entries[0].name, "video");
    EXPECT_EQ(out.entries[0].kind, EntryKind::DIR);
    EXPECT_EQ(out.entries[1].name, "new-build.tar.gz");
    EXPECT_EQ(out.entries[1].size, 340u * 1024 * 1024);
    EXPECT_EQ(out.entries[1].flags, ENTRY_FLAG_NEW);
}

TEST(V2Protocol, EmptyListDirRoundTrip) {
    ListDirResponse r;
    r.path = "/";
    const Frame f = roundtrip(encode_list_dir_response(r));
    const ListDirResponse out = parse_list_dir_response(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.path, "/");
    EXPECT_TRUE(out.entries.empty());
}

TEST(V2Protocol, ListDirRequestRoundTrip) {
    ListDirRequest r{"/a/b/c"};
    const Frame f = roundtrip(encode_list_dir_request(r));
    EXPECT_EQ(parse_list_dir_request(f.payload.data(), f.payload.size()).path, "/a/b/c");
}

TEST(V2Protocol, StatRoundTrip) {
    StatResponse r;
    r.path = "/big.iso";
    r.entry = DirEntry{"big.iso", EntryKind::FILE, 4881539072ull, 1234, 0};
    const Frame f = roundtrip(encode_stat_response(r));
    const StatResponse out = parse_stat_response(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.path, "/big.iso");
    EXPECT_EQ(out.entry.size, 4881539072ull);
    EXPECT_EQ(out.entry.mtime, 1234u);
}

TEST(V2Protocol, ChecksumResponseRoundTrip) {
    ChecksumResponse r;
    r.path = "/f";
    r.algo = ALGO_SHA256;
    r.checksum.fill(0x5A);
    const Frame f = roundtrip(encode_checksum_response(r));
    const ChecksumResponse out = parse_checksum_response(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.algo, ALGO_SHA256);
    EXPECT_EQ(out.checksum, r.checksum);
}

// --- Transfer ---------------------------------------------------------------
TEST(V2Protocol, DownloadRequestRoundTrip) {
    DownloadRequest r{"/video/big.iso", 1073741824ull};
    const Frame f = roundtrip(encode_download_request(r));
    const DownloadRequest out = parse_download_request(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.path, "/video/big.iso");
    EXPECT_EQ(out.offset, 1073741824ull);
}

TEST(V2Protocol, DownloadAcceptRoundTrip) {
    DownloadAccept a{42, 999999ull};
    const Frame f = roundtrip(encode_download_accept(a));
    const DownloadAccept out = parse_download_accept(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.transfer_id, 42u);
    EXPECT_EQ(out.total_size, 999999ull);
}

TEST(V2Protocol, ChunkDataRoundTrip) {
    const std::vector<std::uint8_t> data{9, 8, 7, 6, 5};
    const Frame f = roundtrip(encode_chunk_data(7, data.data(), data.size()));
    ASSERT_EQ(f.type, Msg::CHUNK_DATA);
    const ChunkView v = parse_chunk_data(f.payload.data(), f.payload.size());
    EXPECT_EQ(v.transfer_id, 7u);
    ASSERT_EQ(v.len, data.size());
    EXPECT_EQ(std::vector<std::uint8_t>(v.data, v.data + v.len), data);
}

TEST(V2Protocol, EmptyChunkDataRoundTrip) {
    const Frame f = roundtrip(encode_chunk_data(3, nullptr, 0));
    const ChunkView v = parse_chunk_data(f.payload.data(), f.payload.size());
    EXPECT_EQ(v.transfer_id, 3u);
    EXPECT_EQ(v.len, 0u);
}

TEST(V2Protocol, DownloadDoneRoundTrip) {
    DownloadDone d;
    d.transfer_id = 5;
    d.algo = ALGO_CRC32;
    d.checksum.fill(0x11);
    const Frame f = roundtrip(encode_download_done(d));
    const DownloadDone out = parse_download_done(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.transfer_id, 5u);
    EXPECT_EQ(out.checksum, d.checksum);
}

TEST(V2Protocol, DownloadCancelRoundTrip) {
    const Frame f = roundtrip(encode_download_cancel({8}));
    EXPECT_EQ(parse_download_cancel(f.payload.data(), f.payload.size()).transfer_id, 8u);
}

// --- Events -----------------------------------------------------------------
TEST(V2Protocol, SubscribeRoundTrip) {
    const Frame f = roundtrip(encode_subscribe({SUB_FS | SUB_CONFIG}));
    EXPECT_EQ(parse_subscribe(f.payload.data(), f.payload.size()).mask, SUB_FS | SUB_CONFIG);
}

TEST(V2Protocol, EventFsRoundTrip) {
    EventFs e{FsOp::CREATED, EntryKind::FILE, "/incoming/x.bin", 123, 456};
    const Frame f = roundtrip(encode_event_fs(e));
    const EventFs out = parse_event_fs(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.op, FsOp::CREATED);
    EXPECT_EQ(out.path, "/incoming/x.bin");
    EXPECT_EQ(out.size, 123u);
    EXPECT_EQ(out.mtime, 456u);
}

TEST(V2Protocol, EventNoticeRoundTrip) {
    EventNotice e{Severity::WARN, "shutting down in 60s"};
    const Frame f = roundtrip(encode_event_notice(e));
    const EventNotice out = parse_event_notice(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.severity, Severity::WARN);
    EXPECT_EQ(out.text, "shutting down in 60s");
}

TEST(V2Protocol, EventConfigRoundTrip) {
    EventConfig e{"limits.per_client_bps", "10485760"};
    const Frame f = roundtrip(encode_event_config(e));
    const EventConfig out = parse_event_config(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.key, "limits.per_client_bps");
    EXPECT_EQ(out.new_value, "10485760");
}

// --- Service ----------------------------------------------------------------
TEST(V2Protocol, ErrorRoundTrip) {
    ErrorMessage e{ErrCode::ACCESS_DENIED, "outside share root"};
    const Frame f = roundtrip(encode_error(e));
    const ErrorMessage out = parse_error(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.code, ErrCode::ACCESS_DENIED);
    EXPECT_EQ(out.message, "outside share root");
}

TEST(V2Protocol, PingPong) {
    EXPECT_EQ(roundtrip(encode_ping()).type, Msg::PING);
    EXPECT_EQ(roundtrip(encode_pong()).type, Msg::PONG);
}

// --- Admin ------------------------------------------------------------------
TEST(V2Protocol, AdminConfigRoundTrip) {
    const std::string json = R"({"limits":{"per_client_bps":0}})";
    const Frame f = roundtrip(encode_admin_config(json));
    EXPECT_EQ(parse_admin_config(f.payload.data(), f.payload.size()), json);
}

TEST(V2Protocol, AdminConfigLargeRoundTrip) {
    // Deliberately > 64 KiB to exercise the u32 length prefix.
    const std::string json(100u * 1024, 'x');
    const Frame f = roundtrip(encode_admin_config(json));
    EXPECT_EQ(parse_admin_config(f.payload.data(), f.payload.size()).size(), json.size());
}

TEST(V2Protocol, AdminSetRoundTrip) {
    const Frame f = roundtrip(encode_admin_set({"limits.global_bps", "52428800"}));
    const AdminSet out = parse_admin_set(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.key, "limits.global_bps");
    EXPECT_EQ(out.value, "52428800");
}

TEST(V2Protocol, AdminClientsRoundTrip) {
    AdminClientInfo c1{1, "vit", "1.2.3.4", Role::USER, "/video/big.iso", 500, 12000000};
    AdminClientInfo c2{2, "admin", "5.6.7.8", Role::ADMIN, "", 0, 0};
    const Frame f = roundtrip(encode_admin_clients({c1, c2}));
    const auto out = parse_admin_clients(f.payload.data(), f.payload.size());
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].login, "vit");
    EXPECT_EQ(out[0].current_path, "/video/big.iso");
    EXPECT_EQ(out[0].speed_bps, 12000000u);
    EXPECT_EQ(out[1].role, Role::ADMIN);
}

TEST(V2Protocol, AdminStatsRoundTrip) {
    AdminStats s;
    s.uptime_seconds = 100;
    s.bytes_sent = 999;
    s.per_client_bps = 10485760;
    s.version = "2.0.1";
    const Frame f = roundtrip(encode_admin_stats_response(s));
    const AdminStats out = parse_admin_stats_response(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.uptime_seconds, 100u);
    EXPECT_EQ(out.per_client_bps, 10485760u);
    EXPECT_EQ(out.version, "2.0.1");
}

TEST(V2Protocol, AdminShutdownRoundTrip) {
    const Frame f = roundtrip(encode_admin_shutdown({60}));
    EXPECT_EQ(parse_admin_shutdown(f.payload.data(), f.payload.size()).grace_seconds, 60u);
}

// --- Adversarial: truncated / oversize / unknown ----------------------------
TEST(V2ProtocolReject, UnknownType) {
    const std::vector<std::uint8_t> b{0x99, 0, 0, 0, 0};
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(V2ProtocolReject, V1CodeRejected) {
    // A v1 LIST_REQUEST (0x01) is not a valid v2 type.
    const std::vector<std::uint8_t> b{0x01, 0, 0, 0, 0};
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(V2ProtocolReject, ShortHeader) {
    const std::vector<std::uint8_t> b{0x10, 0, 0};
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(V2ProtocolReject, LengthMismatch) {
    // HELLO header claims 4 payload bytes, none present.
    const std::vector<std::uint8_t> b{static_cast<std::uint8_t>(Msg::HELLO), 0, 0, 0, 4};
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(V2ProtocolReject, PayloadOverCeiling) {
    std::vector<std::uint8_t> b(HEADER_SIZE, 0);
    b[0] = static_cast<std::uint8_t>(Msg::LIST_DIR_RESPONSE);
    const std::uint32_t big = MAX_CONTROL_PAYLOAD + 1;
    b[1] = static_cast<std::uint8_t>((big >> 24) & 0xFF);
    b[2] = static_cast<std::uint8_t>((big >> 16) & 0xFF);
    b[3] = static_cast<std::uint8_t>((big >> 8) & 0xFF);
    b[4] = static_cast<std::uint8_t>(big & 0xFF);
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(V2ProtocolReject, ListDirResponseTruncatedEntry) {
    // path="" (len 0), count=1, but no entry bytes follow.
    const std::vector<std::uint8_t> p{0, 0, 0, 0, 0, 1};
    EXPECT_THROW((void)parse_list_dir_response(p.data(), p.size()), ProtocolError);
}

TEST(V2ProtocolReject, ListDirResponseHugeCount) {
    // path="" , count = 0xFFFFFFFF (> MAX_LIST_ENTRIES) must be rejected before
    // any allocation.
    const std::vector<std::uint8_t> p{0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_THROW((void)parse_list_dir_response(p.data(), p.size()), ProtocolError);
}

TEST(V2ProtocolReject, TrailingBytes) {
    // Valid SUBSCRIBE (4-byte mask) + 1 trailing byte.
    std::vector<std::uint8_t> p{0, 0, 0, 1, 0xFF};
    EXPECT_THROW((void)parse_subscribe(p.data(), p.size()), ProtocolError);
}

TEST(V2ProtocolReject, ChunkDataTooShort) {
    // Fewer than 4 bytes -> no room for transfer_id.
    const std::vector<std::uint8_t> p{1, 2, 3};
    EXPECT_THROW((void)parse_chunk_data(p.data(), p.size()), ProtocolError);
}

TEST(V2ProtocolReject, PathTooLong) {
    ListDirRequest r;
    r.path = std::string(MAX_PATH_LEN + 1, 'a');
    EXPECT_THROW((void)encode_list_dir_request(r), ProtocolError);
}

TEST(V2ProtocolReject, StringLimitOnParse) {
    // path length prefix = MAX_PATH_LEN + 1 declared but reader guards it.
    std::vector<std::uint8_t> p;
    const std::uint32_t declared = static_cast<std::uint32_t>(MAX_PATH_LEN) + 1;
    p.push_back(static_cast<std::uint8_t>((declared >> 8) & 0xFF));  // u16 high
    p.push_back(static_cast<std::uint8_t>(declared & 0xFF));         // u16 low (truncated view is fine)
    EXPECT_THROW((void)parse_list_dir_request(p.data(), p.size()), ProtocolError);
}
