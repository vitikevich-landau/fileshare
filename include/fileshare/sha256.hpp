#pragma once

#include <cstddef>
#include <cstdint>

#include "fileshare/types.hpp"

namespace fileshare {

// Streaming SHA-256 (OpenSSL EVP). Only built when FILESHARE_USE_SHA256 is set.
// value() returns the full 32-byte digest, which exactly fills the Checksum
// field (unlike CRC32, which used only the first 4 bytes). The EVP context is
// held as a void* so this header does not pull in <openssl/*>.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const std::uint8_t* data, std::size_t len);
    [[nodiscard]] Checksum value() const; // non-destructive: finalizes a copy
    void reset();

private:
    void* ctx_;
};

} // namespace fileshare
