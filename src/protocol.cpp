#include "fileshare/protocol.hpp"

#include <cstring>
#include <utility>

namespace fileshare {

// --- Message-type table -----------------------------------------------------
bool is_known_message_type(std::uint8_t raw) noexcept {
    switch (static_cast<MessageType>(raw)) {
        case MessageType::LIST_REQUEST:
        case MessageType::LIST_RESPONSE:
        case MessageType::DOWNLOAD_REQUEST:
        case MessageType::CHUNK_DATA:
        case MessageType::DOWNLOAD_DONE:
        case MessageType::ERROR_MSG:
        case MessageType::PING:
        case MessageType::PONG:
            return true;
    }
    return false;
}

// --- Primitive writers ------------------------------------------------------
void write_u8(std::vector<std::uint8_t>& buf, std::uint8_t v) {
    buf.push_back(v);
}

void write_u16be(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void write_u32be(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void write_u64be(std::vector<std::uint8_t>& buf, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

// --- Primitive readers ------------------------------------------------------
std::uint16_t read_u16be(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

std::uint32_t read_u32be(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
           (static_cast<std::uint32_t>(p[3]));
}

std::uint64_t read_u64be(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    }
    return v;
}

// --- ByteReader -------------------------------------------------------------
void ByteReader::require(std::size_t n) const {
    if (remaining() < n) {
        throw ProtocolError("unexpected end of payload");
    }
}

std::uint8_t ByteReader::u8() {
    require(1);
    return data_[pos_++];
}

std::uint16_t ByteReader::u16be() {
    require(2);
    std::uint16_t v = read_u16be(data_ + pos_);
    pos_ += 2;
    return v;
}

std::uint32_t ByteReader::u32be() {
    require(4);
    std::uint32_t v = read_u32be(data_ + pos_);
    pos_ += 4;
    return v;
}

std::uint64_t ByteReader::u64be() {
    require(8);
    std::uint64_t v = read_u64be(data_ + pos_);
    pos_ += 8;
    return v;
}

std::string ByteReader::str(std::size_t n) {
    require(n);
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
}

void ByteReader::read_into(std::uint8_t* dst, std::size_t n) {
    require(n);
    std::memcpy(dst, data_ + pos_, n);
    pos_ += n;
}

// --- Frames -----------------------------------------------------------------
FrameHeader parse_header(const std::uint8_t* p) {
    const std::uint8_t raw = p[0];
    if (!is_known_message_type(raw)) {
        throw ProtocolError("unknown message type");
    }
    const std::uint32_t len = read_u32be(p + 1);
    if (len > MAX_CONTROL_PAYLOAD) {
        throw ProtocolError("payload length exceeds MAX_CONTROL_PAYLOAD");
    }
    return FrameHeader{static_cast<MessageType>(raw), len};
}

std::vector<std::uint8_t> make_frame(MessageType type,
                                     const std::vector<std::uint8_t>& payload) {
    if (payload.size() > MAX_CONTROL_PAYLOAD) {
        throw ProtocolError("frame payload exceeds MAX_CONTROL_PAYLOAD");
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
        throw ProtocolError("frame shorter than header");
    }
    const FrameHeader h = parse_header(bytes.data());
    if (bytes.size() - HEADER_SIZE != h.payload_len) {
        throw ProtocolError("frame length does not match declared payload");
    }
    Frame f;
    f.type = h.type;
    f.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(HEADER_SIZE), bytes.end());
    return f;
}

// --- Encoders ---------------------------------------------------------------
namespace {

void append_bytes(std::vector<std::uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
}

} // namespace

std::vector<std::uint8_t> encode_list_request() {
    return make_frame(MessageType::LIST_REQUEST, {});
}

std::vector<std::uint8_t> encode_list_response(const std::vector<ListEntry>& entries) {
    std::vector<std::uint8_t> p;
    write_u32be(p, static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        if (e.alias.size() > MAX_ALIAS_LEN) {
            throw ProtocolError("alias exceeds MAX_ALIAS_LEN");
        }
        write_u16be(p, static_cast<std::uint16_t>(e.alias.size()));
        append_bytes(p, e.alias);
        write_u64be(p, e.size);
        p.insert(p.end(), e.checksum.begin(), e.checksum.end());
    }
    return make_frame(MessageType::LIST_RESPONSE, p);
}

std::vector<std::uint8_t> encode_download_request(const DownloadRequest& req) {
    if (req.alias.size() > MAX_ALIAS_LEN) {
        throw ProtocolError("alias exceeds MAX_ALIAS_LEN");
    }
    std::vector<std::uint8_t> p;
    write_u16be(p, static_cast<std::uint16_t>(req.alias.size()));
    append_bytes(p, req.alias);
    write_u64be(p, req.offset);
    return make_frame(MessageType::DOWNLOAD_REQUEST, p);
}

std::vector<std::uint8_t> encode_chunk_data(const std::uint8_t* data, std::size_t len) {
    std::vector<std::uint8_t> p(data, data + len);
    return make_frame(MessageType::CHUNK_DATA, p);
}

std::vector<std::uint8_t> encode_download_done(const Checksum& checksum) {
    std::vector<std::uint8_t> p(checksum.begin(), checksum.end());
    return make_frame(MessageType::DOWNLOAD_DONE, p);
}

std::vector<std::uint8_t> encode_error(const ErrorMessage& err) {
    if (err.message.size() > 0xFFFFu) {
        throw ProtocolError("error message exceeds u16 length");
    }
    std::vector<std::uint8_t> p;
    write_u16be(p, static_cast<std::uint16_t>(err.code));
    write_u16be(p, static_cast<std::uint16_t>(err.message.size()));
    append_bytes(p, err.message);
    return make_frame(MessageType::ERROR_MSG, p);
}

std::vector<std::uint8_t> encode_ping() {
    return make_frame(MessageType::PING, {});
}

std::vector<std::uint8_t> encode_pong() {
    return make_frame(MessageType::PONG, {});
}

// --- Parsers ----------------------------------------------------------------
std::vector<ListEntry> parse_list_response(const std::uint8_t* payload, std::size_t len) {
    ByteReader r(payload, len);
    const std::uint32_t count = r.u32be();
    std::vector<ListEntry> out;
    // Deliberately no reserve(count): a hostile count would OOM. Each iteration
    // reads from a bounded buffer and throws as soon as the payload runs out.
    for (std::uint32_t i = 0; i < count; ++i) {
        ListEntry e;
        const std::uint16_t alias_len = r.u16be();
        if (alias_len > MAX_ALIAS_LEN) {
            throw ProtocolError("alias exceeds MAX_ALIAS_LEN");
        }
        e.alias = r.str(alias_len);
        e.size = r.u64be();
        r.read_into(e.checksum.data(), e.checksum.size());
        out.push_back(std::move(e));
    }
    if (!r.empty()) {
        throw ProtocolError("trailing bytes in LIST_RESPONSE");
    }
    return out;
}

DownloadRequest parse_download_request(const std::uint8_t* payload, std::size_t len) {
    ByteReader r(payload, len);
    DownloadRequest req;
    const std::uint16_t alias_len = r.u16be();
    if (alias_len > MAX_ALIAS_LEN) {
        throw ProtocolError("alias exceeds MAX_ALIAS_LEN");
    }
    req.alias = r.str(alias_len);
    req.offset = r.u64be();
    if (!r.empty()) {
        throw ProtocolError("trailing bytes in DOWNLOAD_REQUEST");
    }
    return req;
}

Checksum parse_download_done(const std::uint8_t* payload, std::size_t len) {
    if (len != CHECKSUM_LEN) {
        throw ProtocolError("DOWNLOAD_DONE payload must be exactly 32 bytes");
    }
    Checksum c{};
    std::memcpy(c.data(), payload, CHECKSUM_LEN);
    return c;
}

ErrorMessage parse_error(const std::uint8_t* payload, std::size_t len) {
    ByteReader r(payload, len);
    ErrorMessage e;
    e.code = static_cast<ErrorCode>(r.u16be());
    const std::uint16_t mlen = r.u16be();
    e.message = r.str(mlen);
    if (!r.empty()) {
        throw ProtocolError("trailing bytes in ERROR");
    }
    return e;
}

} // namespace fileshare
