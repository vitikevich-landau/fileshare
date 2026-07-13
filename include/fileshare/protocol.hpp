#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "fileshare/types.hpp"

namespace fileshare {

enum class MessageType : std::uint8_t {
    LIST_REQUEST     = 0x01,  // client -> server
    LIST_RESPONSE    = 0x02,  // server -> client
    DOWNLOAD_REQUEST = 0x03,  // client -> server
    CHUNK_DATA       = 0x04,  // server -> client (repeated)
    DOWNLOAD_DONE    = 0x05,  // server -> client
    ERROR_MSG        = 0x06,  // either direction (named *_MSG: `ERROR` is a macro in <windows.h>)
    PING             = 0x07,  // client -> server
    PONG             = 0x08,  // server -> client
};

enum class ErrorCode : std::uint16_t {
    OK                 = 0,
    FILE_NOT_FOUND     = 1,
    UNSUPPORTED_OFFSET = 2,
    BAD_REQUEST        = 3,
    INTERNAL_ERROR     = 4,
};

[[nodiscard]] bool is_known_message_type(std::uint8_t raw) noexcept;

// Thrown on any malformed / out-of-bounds / oversize input. Networking code
// catches this at the connection boundary and drops that single connection,
// leaving the rest of the server untouched (§7).
class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& what) : std::runtime_error(what) {}
};

// --- Primitive serialisation (§4.1): explicit, never reinterpret_cast -------
void write_u8   (std::vector<std::uint8_t>& buf, std::uint8_t  v);
void write_u16be(std::vector<std::uint8_t>& buf, std::uint16_t v);
void write_u32be(std::vector<std::uint8_t>& buf, std::uint32_t v);
void write_u64be(std::vector<std::uint8_t>& buf, std::uint64_t v);

[[nodiscard]] std::uint16_t read_u16be(const std::uint8_t* p) noexcept;
[[nodiscard]] std::uint32_t read_u32be(const std::uint8_t* p) noexcept;
[[nodiscard]] std::uint64_t read_u64be(const std::uint8_t* p) noexcept;

// Bounds-checked sequential reader over a payload buffer. Every accessor
// throws ProtocolError on underflow, so parsers never read past the end.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    [[nodiscard]] std::uint8_t  u8();
    [[nodiscard]] std::uint16_t u16be();
    [[nodiscard]] std::uint32_t u32be();
    [[nodiscard]] std::uint64_t u64be();
    [[nodiscard]] std::string   str(std::size_t n);
    void read_into(std::uint8_t* dst, std::size_t n);

    [[nodiscard]] std::size_t remaining() const noexcept { return size_ - pos_; }
    [[nodiscard]] bool        empty() const noexcept { return pos_ == size_; }

private:
    void require(std::size_t n) const;

    const std::uint8_t* data_;
    std::size_t         size_;
    std::size_t         pos_ = 0;
};

// --- Frames -----------------------------------------------------------------
struct FrameHeader {
    MessageType   type{};
    std::uint32_t payload_len = 0;
};

// Parse the 5-byte header. `p` must point to at least HEADER_SIZE bytes.
// Throws ProtocolError on an unknown type or payload_len > MAX_CONTROL_PAYLOAD.
[[nodiscard]] FrameHeader parse_header(const std::uint8_t* p);

// Build a complete frame (header + payload).
[[nodiscard]] std::vector<std::uint8_t> make_frame(MessageType type,
                                                   const std::vector<std::uint8_t>& payload);

struct Frame {
    MessageType               type{};
    std::vector<std::uint8_t> payload;
};

// Decode exactly one complete frame. Throws ProtocolError if the buffer is
// shorter than the header, longer/shorter than the declared payload, has an
// unknown type, or an oversize payload.
[[nodiscard]] Frame decode_frame(const std::vector<std::uint8_t>& bytes);

// --- Message payloads (§4.2) ------------------------------------------------
struct ListEntry {
    std::string   alias;
    std::uint64_t size = 0;
    Checksum      checksum{};
};

struct DownloadRequest {
    std::string   alias;
    std::uint64_t offset = 0;   // 0 = from start, >0 = resume
};

struct ErrorMessage {
    ErrorCode   code = ErrorCode::INTERNAL_ERROR;
    std::string message;
};

// Encoders — each returns a complete frame ready to send.
[[nodiscard]] std::vector<std::uint8_t> encode_list_request();
[[nodiscard]] std::vector<std::uint8_t> encode_list_response(const std::vector<ListEntry>& entries);
[[nodiscard]] std::vector<std::uint8_t> encode_download_request(const DownloadRequest& req);
[[nodiscard]] std::vector<std::uint8_t> encode_chunk_data(const std::uint8_t* data, std::size_t len);
[[nodiscard]] std::vector<std::uint8_t> encode_download_done(const Checksum& checksum);
[[nodiscard]] std::vector<std::uint8_t> encode_error(const ErrorMessage& err);
[[nodiscard]] std::vector<std::uint8_t> encode_ping();
[[nodiscard]] std::vector<std::uint8_t> encode_pong();

// Parsers — operate on a payload (no header). Throw ProtocolError on malformed
// input.
[[nodiscard]] std::vector<ListEntry> parse_list_response(const std::uint8_t* payload, std::size_t len);
[[nodiscard]] DownloadRequest        parse_download_request(const std::uint8_t* payload, std::size_t len);
[[nodiscard]] Checksum               parse_download_done(const std::uint8_t* payload, std::size_t len);
[[nodiscard]] ErrorMessage           parse_error(const std::uint8_t* payload, std::size_t len);

} // namespace fileshare
