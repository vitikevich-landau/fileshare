#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fileshare {

// --- Frame layout (see docs/fileshare_tz.md §4.1) --------------------------
//   [ msg_type : u8 ][ payload_length : u32 big-endian ][ payload : N bytes ]
inline constexpr std::size_t HEADER_SIZE = 5;

// --- Size limits (§4.3) -----------------------------------------------------
inline constexpr std::size_t   MAX_ALIAS_LEN       = 255;         // bytes
inline constexpr std::uint32_t MAX_CONTROL_PAYLOAD = 1u << 20;    // 1 MiB
inline constexpr std::size_t   CHUNK_SIZE          = 64 * 1024;   // 64 KiB

// --- Checksum ---------------------------------------------------------------
// A fixed 32-byte field on the wire and in config.json, chosen for
// forward-compat with SHA-256. For now CRC32 occupies the first 4 bytes
// (big-endian); the remaining 28 bytes stay zero.
inline constexpr std::size_t CHECKSUM_LEN = 32;
using Checksum = std::array<std::uint8_t, CHECKSUM_LEN>;

} // namespace fileshare
