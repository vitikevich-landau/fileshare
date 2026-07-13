#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "fileshare/crc32.hpp"

using namespace fileshare;

namespace {
std::uint32_t crc_str(const std::string& s) {
    return crc32(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}
const std::uint8_t* as_bytes(const char* s) {
    return reinterpret_cast<const std::uint8_t*>(s);
}
} // namespace

TEST(Crc32, KnownVectors) {
    EXPECT_EQ(crc_str(""), 0x00000000u);
    EXPECT_EQ(crc_str("123456789"), 0xCBF43926u);
    EXPECT_EQ(crc_str("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(Crc32, StreamingEqualsOneShot) {
    const std::string s = "The quick brown fox jumps over the lazy dog";
    Crc32 c;
    c.update(as_bytes(s.data()), 10);
    c.update(as_bytes(s.data()) + 10, 10);
    c.update(as_bytes(s.data()) + 20, s.size() - 20);
    EXPECT_EQ(c.value(), crc_str(s));
}

TEST(Crc32, EmptyUpdateIsIdentity) {
    Crc32 c;
    c.update(nullptr, 0);
    EXPECT_EQ(c.value(), 0x00000000u);
}

TEST(Crc32, ResetRestartsState) {
    Crc32 c;
    c.update(as_bytes("abc"), 3);
    c.reset();
    EXPECT_EQ(c.value(), 0x00000000u);
}
