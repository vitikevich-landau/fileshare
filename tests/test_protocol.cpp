#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fileshare/protocol.hpp"

using namespace fileshare;

// --- Primitive serialisation layout -----------------------------------------
TEST(Serialization, U16RoundTripAndLayout) {
    std::vector<std::uint8_t> b;
    write_u16be(b, 0x1234);
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 0x12);
    EXPECT_EQ(b[1], 0x34);
    EXPECT_EQ(read_u16be(b.data()), 0x1234u);
}

TEST(Serialization, U32Layout) {
    std::vector<std::uint8_t> b;
    write_u32be(b, 0xDEADBEEFu);
    const std::array<std::uint8_t, 4> expected{0xDE, 0xAD, 0xBE, 0xEF};
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(b[i], expected[i]);
    }
    EXPECT_EQ(read_u32be(b.data()), 0xDEADBEEFu);
}

TEST(Serialization, U64Layout) {
    std::vector<std::uint8_t> b;
    write_u64be(b, 0x0102030405060708ull);
    for (std::size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(b[i], static_cast<std::uint8_t>(i + 1));
    }
    EXPECT_EQ(read_u64be(b.data()), 0x0102030405060708ull);
}

// --- Frame round-trips ------------------------------------------------------
TEST(Frame, ListRequestRoundTrip) {
    const auto bytes = encode_list_request();
    ASSERT_EQ(bytes.size(), HEADER_SIZE);      // empty payload
    const Frame f = decode_frame(bytes);
    EXPECT_EQ(f.type, MessageType::LIST_REQUEST);
    EXPECT_TRUE(f.payload.empty());
}

TEST(Frame, ListResponseRoundTrip) {
    ListEntry e1;
    e1.alias = "ubuntu-24.04.iso";
    e1.size = 4881539072ull;
    e1.checksum.fill(0xAB);
    ListEntry e2;   // second entry with zeroed checksum and empty-ish alias
    e2.alias = "a";
    e2.size = 0;

    const auto bytes = encode_list_response({e1, e2});
    const Frame f = decode_frame(bytes);
    EXPECT_EQ(f.type, MessageType::LIST_RESPONSE);

    const auto out = parse_list_response(f.payload.data(), f.payload.size());
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].alias, "ubuntu-24.04.iso");
    EXPECT_EQ(out[0].size, 4881539072ull);
    EXPECT_EQ(out[0].checksum, e1.checksum);
    EXPECT_EQ(out[1].alias, "a");
    EXPECT_EQ(out[1].size, 0u);
    EXPECT_EQ(out[1].checksum, e2.checksum);
}

TEST(Frame, EmptyListResponseRoundTrip) {
    const auto out = parse_list_response(
        decode_frame(encode_list_response({})).payload.data(), 4);
    EXPECT_TRUE(out.empty());
}

TEST(Frame, DownloadRequestRoundTrip) {
    DownloadRequest req;
    req.alias = "file.bin";
    req.offset = 123456789ull;
    const Frame f = decode_frame(encode_download_request(req));
    EXPECT_EQ(f.type, MessageType::DOWNLOAD_REQUEST);
    const auto out = parse_download_request(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.alias, "file.bin");
    EXPECT_EQ(out.offset, 123456789ull);
}

TEST(Frame, DownloadDoneRoundTrip) {
    Checksum c;
    c.fill(0x7F);
    const Frame f = decode_frame(encode_download_done(c));
    EXPECT_EQ(f.type, MessageType::DOWNLOAD_DONE);
    EXPECT_EQ(parse_download_done(f.payload.data(), f.payload.size()), c);
}

TEST(Frame, ErrorRoundTrip) {
    ErrorMessage err;
    err.code = ErrorCode::FILE_NOT_FOUND;
    err.message = "no such alias";
    const Frame f = decode_frame(encode_error(err));
    EXPECT_EQ(f.type, MessageType::ERROR_MSG);
    const auto out = parse_error(f.payload.data(), f.payload.size());
    EXPECT_EQ(out.code, ErrorCode::FILE_NOT_FOUND);
    EXPECT_EQ(out.message, "no such alias");
}

TEST(Frame, ChunkDataRoundTrip) {
    const std::vector<std::uint8_t> data{1, 2, 3, 4, 5};
    const Frame f = decode_frame(encode_chunk_data(data.data(), data.size()));
    EXPECT_EQ(f.type, MessageType::CHUNK_DATA);
    EXPECT_EQ(f.payload, data);
}

TEST(Frame, PingPongRoundTrip) {
    EXPECT_EQ(decode_frame(encode_ping()).type, MessageType::PING);
    EXPECT_EQ(decode_frame(encode_pong()).type, MessageType::PONG);
}

// --- Rejections (adversarial: truncated / oversize / malformed) -------------
TEST(FrameReject, TooShortForHeader) {
    const std::vector<std::uint8_t> b{0x01, 0x00, 0x00};
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(FrameReject, UnknownType) {
    const std::vector<std::uint8_t> b{0xFF, 0, 0, 0, 0};
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(FrameReject, LengthMismatchTooFew) {
    // header claims 4 payload bytes, only 1 present
    const std::vector<std::uint8_t> b{
        static_cast<std::uint8_t>(MessageType::CHUNK_DATA), 0, 0, 0, 4, 0xAA};
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(FrameReject, LengthMismatchTooMany) {
    // header claims 0 payload bytes, but a trailing byte is present
    const std::vector<std::uint8_t> b{
        static_cast<std::uint8_t>(MessageType::PING), 0, 0, 0, 0, 0xAA};
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(FrameReject, PayloadTooLarge) {
    std::vector<std::uint8_t> b(HEADER_SIZE, 0);
    b[0] = static_cast<std::uint8_t>(MessageType::CHUNK_DATA);
    const std::uint32_t big = MAX_CONTROL_PAYLOAD + 1;
    b[1] = static_cast<std::uint8_t>((big >> 24) & 0xFF);
    b[2] = static_cast<std::uint8_t>((big >> 16) & 0xFF);
    b[3] = static_cast<std::uint8_t>((big >> 8) & 0xFF);
    b[4] = static_cast<std::uint8_t>(big & 0xFF);
    EXPECT_THROW((void)decode_frame(b), ProtocolError);
}

TEST(ParseReject, ListResponseTruncated) {
    // count = 1 but no entry bytes follow
    const std::vector<std::uint8_t> p{0, 0, 0, 1};
    EXPECT_THROW((void)parse_list_response(p.data(), p.size()), ProtocolError);
}

TEST(ParseReject, ListResponseAliasTooLong) {
    // count = 1, alias_len = 300 (> MAX_ALIAS_LEN)
    const std::vector<std::uint8_t> p{0, 0, 0, 1, 0x01, 0x2C};
    EXPECT_THROW((void)parse_list_response(p.data(), p.size()), ProtocolError);
}

TEST(ParseReject, ListResponseTrailingBytes) {
    // count = 0 but extra bytes remain
    const std::vector<std::uint8_t> p{0, 0, 0, 0, 0xFF};
    EXPECT_THROW((void)parse_list_response(p.data(), p.size()), ProtocolError);
}

TEST(ParseReject, DownloadDoneWrongSize) {
    const std::vector<std::uint8_t> p(16, 0);
    EXPECT_THROW((void)parse_download_done(p.data(), p.size()), ProtocolError);
}

TEST(ParseReject, DownloadRequestTrailingBytes) {
    // alias_len = 1 + 'x' + 8-byte offset + 1 extra byte
    std::vector<std::uint8_t> p{0x00, 0x01, 'x', 0, 0, 0, 0, 0, 0, 0, 0, 0xFF};
    EXPECT_THROW((void)parse_download_request(p.data(), p.size()), ProtocolError);
}

TEST(EncodeReject, AliasTooLongInDownloadRequest) {
    DownloadRequest req;
    req.alias = std::string(MAX_ALIAS_LEN + 1, 'x');
    EXPECT_THROW((void)encode_download_request(req), ProtocolError);
}
