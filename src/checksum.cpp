#include "fileshare/checksum.hpp"

#include <fstream>
#include <vector>

#include "fileshare/crc32.hpp"

namespace fileshare {

Checksum checksum_from_crc32(std::uint32_t crc) noexcept {
    Checksum sum{};   // zero-initialised
    sum[0] = static_cast<std::uint8_t>((crc >> 24) & 0xFFu);
    sum[1] = static_cast<std::uint8_t>((crc >> 16) & 0xFFu);
    sum[2] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
    sum[3] = static_cast<std::uint8_t>(crc & 0xFFu);
    return sum;
}

std::string to_hex(const Checksum& sum) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(sum.size() * 2);
    for (std::uint8_t b : sum) {
        out.push_back(kHex[(b >> 4) & 0x0Fu]);
        out.push_back(kHex[b & 0x0Fu]);
    }
    return out;
}

std::optional<Checksum> checksum_from_hex(std::string_view hex) {
    if (hex.size() != CHECKSUM_LEN * 2) {
        return std::nullopt;
    }
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    Checksum sum{};
    for (std::size_t i = 0; i < CHECKSUM_LEN; ++i) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        sum[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return sum;
}

FileDigest compute_file_digest_crc32(const std::string& path) {
    FileDigest result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "cannot open file: " + path;
        return result;
    }

    Crc32 crc;
    std::vector<char> buf(64 * 1024);
    std::uint64_t total = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            crc.update(reinterpret_cast<const std::uint8_t*>(buf.data()),
                       static_cast<std::size_t>(got));
            total += static_cast<std::uint64_t>(got);
        }
    }
    if (in.bad()) {
        result.error = "read error: " + path;
        return result;
    }

    result.ok = true;
    result.size = total;
    result.checksum = checksum_from_crc32(crc.value());
    result.algo = "crc32";
    return result;
}

} // namespace fileshare
