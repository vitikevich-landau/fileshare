#include <gtest/gtest.h>

#ifdef FILESHARE_USE_SHA256

#include <cstdint>
#include <string>

#include "fileshare/checksum.hpp"
#include "fileshare/sha256.hpp"

using namespace fileshare;

namespace {
const std::uint8_t* bytes(const std::string& s) {
    return reinterpret_cast<const std::uint8_t*>(s.data());
}
Checksum sha_str(const std::string& s) {
    Sha256 h;
    h.update(bytes(s), s.size());
    return h.value();
}
} // namespace

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(to_hex(sha_str("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(to_hex(sha_str("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, StreamingEqualsOneShot) {
    const std::string s = "The quick brown fox jumps over the lazy dog";
    Sha256 h;
    h.update(bytes(s), 10);
    h.update(bytes(s) + 10, s.size() - 10);
    EXPECT_EQ(h.value(), sha_str(s));
}

TEST(Sha256, ValueIsNonDestructive) {
    Sha256 h;
    h.update(bytes("abc"), 3);
    const Checksum first = h.value();
    const Checksum second = h.value(); // computing twice must match
    EXPECT_EQ(first, second);
}

TEST(Sha256, FileHasherUsesSha256) {
    EXPECT_STREQ(FileHasher::algorithm(), "sha256");
    FileHasher fh;
    fh.update(bytes("abc"), 3);
    EXPECT_EQ(fh.value(), sha_str("abc"));
}

#endif // FILESHARE_USE_SHA256
