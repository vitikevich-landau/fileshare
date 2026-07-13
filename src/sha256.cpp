#include "fileshare/sha256.hpp"

#include <stdexcept>

#include <openssl/evp.h>

namespace fileshare {

Sha256::Sha256() : ctx_(EVP_MD_CTX_new()) {
    if (ctx_ == nullptr) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }
    if (EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
        throw std::runtime_error("EVP_DigestInit_ex failed");
    }
}

Sha256::~Sha256() {
    EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
}

void Sha256::update(const std::uint8_t* data, std::size_t len) {
    if (len == 0) {
        return;
    }
    if (EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data, len) != 1) {
        throw std::runtime_error("EVP_DigestUpdate failed");
    }
}

Checksum Sha256::value() const {
    // Finalize a copy so the object stays usable afterwards (matches Crc32).
    EVP_MD_CTX* copy = EVP_MD_CTX_new();
    if (copy == nullptr) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }
    if (EVP_MD_CTX_copy_ex(copy, static_cast<const EVP_MD_CTX*>(ctx_)) != 1) {
        EVP_MD_CTX_free(copy);
        throw std::runtime_error("EVP_MD_CTX_copy_ex failed");
    }
    Checksum out{};
    unsigned int out_len = 0;
    const int ok = EVP_DigestFinal_ex(copy, out.data(), &out_len);
    EVP_MD_CTX_free(copy);
    if (ok != 1 || out_len != CHECKSUM_LEN) {
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }
    return out;
}

void Sha256::reset() {
    if (EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("EVP_DigestInit_ex failed");
    }
}

} // namespace fileshare
