#pragma once

// Blocking v2 frame transport over a net::Socket. The v1 net::send_message /
// recv_message are tied to v1's MessageType table (they reject v2 codes), so v2
// gets its own thin framing layer built on the same recv_exact/send_all
// primitives.

#include <optional>
#include <vector>

#include "fileshare/net.hpp"
#include "fileshare/v2/protocol.hpp"

namespace fileshare::v2 {

// Send a complete frame (header + payload, as produced by the encoders).
inline void send_frame(net::Socket& s, const std::vector<std::uint8_t>& frame) {
    net::send_all(s, frame.data(), frame.size());
}

// Receive one complete frame. Returns std::nullopt on a clean connection close
// at a frame boundary (0 bytes before the header). Throws ProtocolError on a
// truncated/oversize/unknown frame; net::NetError on a socket error.
[[nodiscard]] std::optional<Frame> recv_frame(net::Socket& s);

} // namespace fileshare::v2
