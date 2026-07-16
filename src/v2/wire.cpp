#include "fileshare/v2/wire.hpp"

#include "fileshare/types.hpp"

namespace fileshare::v2 {

std::optional<Frame> recv_frame(net::Socket& s) {
    std::uint8_t header[HEADER_SIZE];
    const std::size_t got = net::recv_exact(s, header, HEADER_SIZE);
    if (got == 0) {
        return std::nullopt;   // clean close on a frame boundary
    }
    if (got < HEADER_SIZE) {
        throw ProtocolError("truncated v2 frame header");
    }

    const FrameHeaderV2 h = parse_header(header);   // validates type + payload ceiling

    Frame f;
    f.type = h.type;
    if (h.payload_len > 0) {
        f.payload.resize(h.payload_len);
        const std::size_t body = net::recv_exact(s, f.payload.data(), h.payload_len);
        if (body < h.payload_len) {
            throw ProtocolError("truncated v2 frame payload");
        }
    }
    return f;
}

} // namespace fileshare::v2
