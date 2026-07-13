#pragma once

#include <cstddef>
#include <cstdint>

namespace fileshare {

// CRC-32 (IEEE 802.3): reflected input/output, polynomial 0xEDB88320,
// init and final-xor 0xFFFFFFFF. crc32("123456789") == 0xCBF43926.
//
// Streaming interface so large files can be hashed without loading them
// fully into memory.
class Crc32 {
public:
    void update(const std::uint8_t* data, std::size_t len) noexcept;
    [[nodiscard]] std::uint32_t value() const noexcept;
    void reset() noexcept;

private:
    std::uint32_t state_ = 0xFFFFFFFFu;
};

// One-shot convenience wrapper.
[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t len) noexcept;

} // namespace fileshare
