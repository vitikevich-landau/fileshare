#include "fileshare/crc32.hpp"

#include <array>

namespace fileshare {
namespace {

// Precomputed CRC-32 lookup table, built once at compile time.
constexpr std::array<std::uint32_t, 256> make_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

} // namespace

void Crc32::update(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint32_t c = state_;
    for (std::size_t i = 0; i < len; ++i) {
        c = kTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    state_ = c;
}

std::uint32_t Crc32::value() const noexcept {
    return state_ ^ 0xFFFFFFFFu;
}

void Crc32::reset() noexcept {
    state_ = 0xFFFFFFFFu;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t len) noexcept {
    Crc32 c;
    c.update(data, len);
    return c.value();
}

} // namespace fileshare
