#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fileshare/v2/auth.hpp"
#include "fileshare/v2/crypto.hpp"

using namespace fileshare::v2;
using namespace fileshare::v2::crypto;

namespace {
std::string hex(const Digest& d) { return to_hex(d.data(), d.size()); }
std::vector<std::uint8_t> bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}
} // namespace

// --- SHA-256 known-answer (FIPS 180-4 examples) -----------------------------
TEST(Crypto, Sha256EmptyVector) {
    EXPECT_EQ(hex(sha256("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Crypto, Sha256AbcVector) {
    EXPECT_EQ(hex(sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Crypto, Sha256LongVector) {
    EXPECT_EQ(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Crypto, Sha256MultiBlockStreaming) {
    // 1,000,000 'a' -> known digest; also exercises streaming across blocks.
    Sha256 h;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size());
    EXPECT_EQ(hex(h.final()),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// --- HMAC-SHA-256 known-answer (RFC 4231 test case 2) -----------------------
TEST(Crypto, HmacRfc4231Case2) {
    // key = "Jefe", data = "what do ya want for nothing?"
    const auto key = bytes("Jefe");
    const auto msg = bytes("what do ya want for nothing?");
    const Digest d = hmac_sha256(key, msg);
    EXPECT_EQ(hex(d), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// --- PBKDF2-HMAC-SHA-256 known-answer (RFC 7914 §11) ------------------------
TEST(Crypto, Pbkdf2Vector) {
    const std::vector<std::uint8_t> salt = bytes("salt");
    const auto dk = pbkdf2_hmac_sha256("passwd", salt, 1, 64);
    ASSERT_EQ(dk.size(), 64u);
    EXPECT_EQ(to_hex(dk.data(), dk.size()).substr(0, 32), "55ac046e56e3089fec1691c22544b605");
}

TEST(Crypto, Pbkdf2VectorIterations) {
    // RFC 7914 §11: P="Password", S="NaCl", c=80000, dkLen=64.
    const std::vector<std::uint8_t> salt = bytes("NaCl");
    const auto dk = pbkdf2_hmac_sha256("Password", salt, 80000, 64);
    ASSERT_EQ(dk.size(), 64u);
    EXPECT_EQ(to_hex(dk.data(), dk.size()).substr(0, 32), "4ddcd8f60b98be21830cee5ef22701f9");
}

// --- Utilities --------------------------------------------------------------
TEST(Crypto, HexRoundTrip) {
    const std::vector<std::uint8_t> v{0x00, 0xFF, 0x10, 0xAB, 0xCD};
    EXPECT_EQ(from_hex(to_hex(v.data(), v.size())), v);
    EXPECT_TRUE(from_hex("xyz").empty());   // invalid
    EXPECT_TRUE(from_hex("abc").empty());   // odd length
}

TEST(Crypto, ConstantTimeEqual) {
    const std::vector<std::uint8_t> a{1, 2, 3, 4};
    const std::vector<std::uint8_t> b{1, 2, 3, 4};
    const std::vector<std::uint8_t> c{1, 2, 3, 5};
    EXPECT_TRUE(constant_time_equal(a.data(), b.data(), a.size()));
    EXPECT_FALSE(constant_time_equal(a.data(), c.data(), a.size()));
}

TEST(Crypto, RandomBytesDiffer) {
    std::uint8_t a[32], b[32];
    random_bytes(a, 32);
    random_bytes(b, 32);
    EXPECT_NE(std::vector<std::uint8_t>(a, a + 32), std::vector<std::uint8_t>(b, b + 32));
}

// --- Challenge-response auth ------------------------------------------------
// Low iteration count: these tests exercise the SCRAM XOR/hash logic, not KDF
// strength (that's covered by the PBKDF2 known-answer tests above).
namespace { constexpr std::uint32_t T_ITERS = 2048; }

TEST(Auth, ChallengeResponseRoundTrip) {
    const User u = make_user("vit", Role::ADMIN, "s3cret-pass", T_ITERS);
    Challenge ch{};
    random_bytes(ch.data(), ch.size());
    const Proof proof = compute_client_proof("s3cret-pass", "vit", ch, T_ITERS);
    EXPECT_TRUE(verify_client_proof(u, ch, proof, T_ITERS));
}

TEST(Auth, WrongPasswordRejected) {
    const User u = make_user("vit", Role::USER, "correct", T_ITERS);
    Challenge ch{};
    random_bytes(ch.data(), ch.size());
    const Proof proof = compute_client_proof("WRONG", "vit", ch, T_ITERS);
    EXPECT_FALSE(verify_client_proof(u, ch, proof, T_ITERS));
}

TEST(Auth, ReplayOnDifferentChallengeFails) {
    const User u = make_user("vit", Role::USER, "pw", T_ITERS);
    Challenge ch1{}, ch2{};
    random_bytes(ch1.data(), ch1.size());
    random_bytes(ch2.data(), ch2.size());
    const Proof proof = compute_client_proof("pw", "vit", ch1, T_ITERS);
    // A proof captured for ch1 must not validate against a fresh ch2.
    EXPECT_FALSE(verify_client_proof(u, ch2, proof, T_ITERS));
}

TEST(Auth, DisabledUserRejected) {
    User u = make_user("vit", Role::USER, "pw", T_ITERS);
    u.enabled = false;
    Challenge ch{};
    random_bytes(ch.data(), ch.size());
    const Proof proof = compute_client_proof("pw", "vit", ch, T_ITERS);
    EXPECT_FALSE(verify_client_proof(u, ch, proof, T_ITERS));
}

TEST(Auth, StolenStoredKeyCannotForgeProof) {
    // An attacker who steals StoredKey (but not the password) cannot craft a
    // proof: they lack ClientKey. The best they can do without it (zeros) fails.
    const User u = make_user("vit", Role::USER, "pw", T_ITERS);
    Challenge ch{};
    random_bytes(ch.data(), ch.size());
    Proof forged{};   // all zeros
    EXPECT_FALSE(verify_client_proof(u, ch, forged, T_ITERS));
}

TEST(Auth, WrongLoginSaltFails) {
    // Proof derived under a different login (different salt) must not verify.
    const User u = make_user("vit", Role::USER, "pw", T_ITERS);
    Challenge ch{};
    random_bytes(ch.data(), ch.size());
    const Proof proof = compute_client_proof("pw", "someone-else", ch, T_ITERS);
    EXPECT_FALSE(verify_client_proof(u, ch, proof, T_ITERS));
}

TEST(Auth, UserDbRoundTrip) {
    const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                            "/fileshare_users_test.json";
    {
        UserDb db;
        db.set(make_user("admin", Role::ADMIN, "adminpw", T_ITERS));
        db.set(make_user("bob", Role::USER, "bobpw", T_ITERS));
        db.save(tmp);
    }
    const UserDb db = UserDb::load(tmp);
    EXPECT_EQ(db.size(), 2u);
    ASSERT_TRUE(db.find("admin").has_value());
    EXPECT_EQ(db.find("admin")->role, Role::ADMIN);

    // Verify a loaded user still authenticates.
    Challenge ch{};
    random_bytes(ch.data(), ch.size());
    EXPECT_TRUE(verify_client_proof(*db.find("bob"), ch, compute_client_proof("bobpw", "bob", ch, T_ITERS), T_ITERS));
    EXPECT_FALSE(verify_client_proof(*db.find("bob"), ch, compute_client_proof("nope", "bob", ch, T_ITERS), T_ITERS));
    std::remove(tmp.c_str());
}

TEST(Auth, MissingUsersFileIsEmpty) {
    const UserDb db = UserDb::load("/nonexistent/users.json");
    EXPECT_TRUE(db.empty());
}
