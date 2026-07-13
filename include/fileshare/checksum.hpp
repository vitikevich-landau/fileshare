#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fileshare/types.hpp"

namespace fileshare {

// Pack a CRC32 into the fixed 32-byte checksum field (big-endian, first 4
// bytes; the rest stay zero).
[[nodiscard]] Checksum checksum_from_crc32(std::uint32_t crc) noexcept;

// Lowercase hex of the full 32-byte field (64 characters).
[[nodiscard]] std::string to_hex(const Checksum& sum);

// Parse 64 hex characters back into a checksum. Returns std::nullopt on wrong
// length or a non-hex character.
[[nodiscard]] std::optional<Checksum> checksum_from_hex(std::string_view hex);

struct FileDigest {
    bool          ok = false;
    std::string   error;          // populated when !ok
    std::uint64_t size = 0;
    Checksum      checksum{};
    std::string   algo = "crc32";
};

// Stream a file from disk and compute its size + CRC32 checksum in one pass.
[[nodiscard]] FileDigest compute_file_digest_crc32(const std::string& path);

} // namespace fileshare
