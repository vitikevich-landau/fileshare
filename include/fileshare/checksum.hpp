#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fileshare/types.hpp"

#ifdef FILESHARE_USE_SHA256
#  include "fileshare/sha256.hpp"
#else
#  include "fileshare/crc32.hpp"
#endif

namespace fileshare {

// Pack a CRC32 into the fixed 32-byte checksum field (big-endian, first 4
// bytes). Kept for the CRC32 build and its tests.
[[nodiscard]] Checksum checksum_from_crc32(std::uint32_t crc) noexcept;

// Lowercase hex of the full 32-byte field (64 characters).
[[nodiscard]] std::string to_hex(const Checksum& sum);

// Parse 64 hex characters back into a checksum. std::nullopt on wrong length or
// a non-hex character.
[[nodiscard]] std::optional<Checksum> checksum_from_hex(std::string_view hex);

// Streaming file-integrity hasher: SHA-256 when built with FILESHARE_USE_SHA256
// (fills all 32 bytes), otherwise CRC32 (first 4 bytes). Streaming so a resumed
// download can be verified across the already-present bytes plus the new ones.
class FileHasher {
public:
    void update(const std::uint8_t* data, std::size_t len);
    [[nodiscard]] Checksum value() const;
    [[nodiscard]] static const char* algorithm() noexcept;

private:
#ifdef FILESHARE_USE_SHA256
    Sha256 impl_;
#else
    Crc32 impl_;
#endif
};

struct FileDigest {
    bool          ok = false;
    std::string   error; // populated when !ok
    std::uint64_t size = 0;
    Checksum      checksum{};
    std::string   algo = FileHasher::algorithm();
};

// Stream a file from disk and compute its size + checksum in one pass, using
// whichever algorithm this build selected.
[[nodiscard]] FileDigest compute_file_digest(const std::string& path);

} // namespace fileshare
