#include "fileshare/v2/crypto.hpp"

#include <cstring>
#include <random>

namespace fileshare::v2::crypto {

// --- SHA-256 (FIPS 180-4) ---------------------------------------------------
namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

} // namespace

void Sha256::reset() {
    state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85; state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f; state_[5] = 0x9b05688c; state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
    bitlen_ = 0;
    buflen_ = 0;
}

void Sha256::process(const std::uint8_t* block) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t len) {
    bitlen_ += static_cast<std::uint64_t>(len) * 8;
    while (len > 0) {
        const std::size_t take = std::min<std::size_t>(64 - buflen_, len);
        std::memcpy(buf_ + buflen_, data, take);
        buflen_ += take;
        data += take;
        len -= take;
        if (buflen_ == 64) {
            process(buf_);
            buflen_ = 0;
        }
    }
}

Digest Sha256::final() {
    // Pad: 0x80, zeros, then 64-bit big-endian bit length.
    const std::uint64_t bitlen = bitlen_;
    std::uint8_t pad = 0x80;
    update(&pad, 1);
    std::uint8_t zero = 0;
    while (buflen_ != 56) {
        update(&zero, 1);
    }
    std::uint8_t lenbuf[8];
    for (int i = 0; i < 8; ++i) {
        lenbuf[i] = static_cast<std::uint8_t>((bitlen >> (56 - i * 8)) & 0xFF);
    }
    // update() would re-add to bitlen_; write the final block directly instead.
    std::memcpy(buf_ + buflen_, lenbuf, 8);
    process(buf_);

    Digest out;
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFF);
    }
    return out;
}

Digest sha256(const std::uint8_t* data, std::size_t len) {
    Sha256 h;
    h.update(data, len);
    return h.final();
}

Digest sha256(const std::string& s) {
    return sha256(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// --- HMAC-SHA-256 -----------------------------------------------------------
Digest hmac_sha256(const std::uint8_t* key, std::size_t key_len,
                   const std::uint8_t* msg, std::size_t msg_len) {
    std::uint8_t k[64];
    std::memset(k, 0, sizeof(k));
    if (key_len > 64) {
        const Digest kh = sha256(key, key_len);
        std::memcpy(k, kh.data(), kh.size());
    } else {
        std::memcpy(k, key, key_len);
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad, 64);
    inner.update(msg, msg_len);
    const Digest inner_d = inner.final();

    Sha256 outer;
    outer.update(opad, 64);
    outer.update(inner_d.data(), inner_d.size());
    return outer.final();
}

Digest hmac_sha256(const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& msg) {
    return hmac_sha256(key.data(), key.size(), msg.data(), msg.size());
}

// --- PBKDF2-HMAC-SHA-256 ----------------------------------------------------
std::vector<std::uint8_t> pbkdf2_hmac_sha256(const std::string& password,
                                             const std::vector<std::uint8_t>& salt,
                                             std::uint32_t iterations, std::size_t dk_len) {
    const auto* pw = reinterpret_cast<const std::uint8_t*>(password.data());
    const std::size_t pw_len = password.size();
    std::vector<std::uint8_t> out;
    out.reserve(dk_len);

    std::uint32_t block_index = 1;
    while (out.size() < dk_len) {
        // U1 = HMAC(pw, salt || INT_32_BE(block_index))
        std::vector<std::uint8_t> salted = salt;
        salted.push_back(static_cast<std::uint8_t>((block_index >> 24) & 0xFF));
        salted.push_back(static_cast<std::uint8_t>((block_index >> 16) & 0xFF));
        salted.push_back(static_cast<std::uint8_t>((block_index >> 8) & 0xFF));
        salted.push_back(static_cast<std::uint8_t>(block_index & 0xFF));

        Digest u = hmac_sha256(pw, pw_len, salted.data(), salted.size());
        Digest t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = hmac_sha256(pw, pw_len, u.data(), u.size());
            for (std::size_t j = 0; j < t.size(); ++j) {
                t[j] = static_cast<std::uint8_t>(t[j] ^ u[j]);
            }
        }
        const std::size_t take = std::min<std::size_t>(t.size(), dk_len - out.size());
        out.insert(out.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
        ++block_index;
    }
    return out;
}

// --- Random -----------------------------------------------------------------
void random_bytes(std::uint8_t* out, std::size_t n) {
    std::random_device rd;
    std::size_t i = 0;
    while (i < n) {
        const std::uint32_t r = rd();
        for (int b = 0; b < 4 && i < n; ++b, ++i) {
            out[i] = static_cast<std::uint8_t>((r >> (b * 8)) & 0xFF);
        }
    }
}

// --- Utilities --------------------------------------------------------------
bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < n; ++i) {
        diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

std::string to_hex(const std::uint8_t* data, std::size_t len) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(d[data[i] >> 4]);
        out.push_back(d[data[i] & 0x0F]);
    }
    return out;
}

std::vector<std::uint8_t> from_hex(const std::string& hex) {
    std::vector<std::uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) { out.clear(); return out; }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace fileshare::v2::crypto
