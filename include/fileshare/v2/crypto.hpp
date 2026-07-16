#pragma once

// Self-contained crypto primitives for authentication (see docs/v2/06-security.md).
// Deliberately dependency-free SHA-256 so the DEFAULT build (CRC32 checksums, no
// OpenSSL) can still authenticate. This is the documented PBKDF2 fallback path;
// swapping in libsodium/Argon2id later only touches this module.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fileshare::v2::crypto {

using Digest = std::array<std::uint8_t, 32>;

// Streaming SHA-256.
class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const std::uint8_t* data, std::size_t len);
    [[nodiscard]] Digest final();   // finalises (consumes) this hasher

private:
    void process(const std::uint8_t* block);
    std::uint32_t   state_[8];
    std::uint64_t   bitlen_ = 0;
    std::uint8_t    buf_[64];
    std::size_t     buflen_ = 0;
};

[[nodiscard]] Digest sha256(const std::uint8_t* data, std::size_t len);
[[nodiscard]] Digest sha256(const std::string& s);

// HMAC-SHA-256 (RFC 2104).
[[nodiscard]] Digest hmac_sha256(const std::uint8_t* key, std::size_t key_len,
                                 const std::uint8_t* msg, std::size_t msg_len);
[[nodiscard]] Digest hmac_sha256(const std::vector<std::uint8_t>& key,
                                 const std::vector<std::uint8_t>& msg);

// PBKDF2-HMAC-SHA-256 (RFC 2898). dk_len bytes of derived key.
[[nodiscard]] std::vector<std::uint8_t> pbkdf2_hmac_sha256(
    const std::string& password, const std::vector<std::uint8_t>& salt,
    std::uint32_t iterations, std::size_t dk_len);

// Cryptographically-seeded random bytes (challenges, salts).
void random_bytes(std::uint8_t* out, std::size_t n);

// Constant-time comparison of equal-length buffers.
[[nodiscard]] bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t n);

// Hex helpers.
[[nodiscard]] std::string to_hex(const std::uint8_t* data, std::size_t len);
[[nodiscard]] std::vector<std::uint8_t> from_hex(const std::string& hex);

} // namespace fileshare::v2::crypto
