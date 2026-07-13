#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "fileshare/checksum.hpp"
#include "fileshare/config.hpp"

using namespace fileshare;
namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const std::string& name) {
    const fs::path dir = fs::path(::testing::TempDir()) / ("fileshare_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
    out.close();
    return p.string();
}

} // namespace

TEST(Config, AddComputesSizeAndChecksum) {
    const auto dir = make_temp_dir("add");
    const std::string fpath = write_file(dir / "hello.txt", "123456789");

    Catalog cat;
    const auto r = cat.add(fpath);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.entry.alias, "hello.txt");
    EXPECT_EQ(r.entry.size_bytes, 9u);
    EXPECT_EQ(r.entry.checksum_algo, "crc32");
    EXPECT_EQ(r.entry.checksum, checksum_from_crc32(0xCBF43926u));
    EXPECT_EQ(cat.size(), 1u);
}

TEST(Config, AddWithExplicitAlias) {
    const auto dir = make_temp_dir("alias");
    const std::string fpath = write_file(dir / "data.bin", "abc");

    Catalog cat;
    const auto r = cat.add(fpath, std::string("myalias"));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.entry.alias, "myalias");
    EXPECT_TRUE(cat.find("myalias").has_value());
    EXPECT_FALSE(cat.find("data.bin").has_value());
}

TEST(Config, AddDuplicateAliasFails) {
    const auto dir = make_temp_dir("dup");
    const std::string fpath = write_file(dir / "x.txt", "hello");

    Catalog cat;
    ASSERT_TRUE(cat.add(fpath).ok);
    const auto r2 = cat.add(fpath);
    EXPECT_FALSE(r2.ok);
    EXPECT_FALSE(r2.error.empty());
    EXPECT_EQ(cat.size(), 1u);
}

TEST(Config, AddMissingFileFails) {
    Catalog cat;
    const auto r = cat.add("definitely/not/here/file.xyz");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(cat.size(), 0u);
}

TEST(Config, RemoveWorks) {
    const auto dir = make_temp_dir("rm");
    const std::string fpath = write_file(dir / "y.txt", "hello");

    Catalog cat;
    ASSERT_TRUE(cat.add(fpath).ok);
    EXPECT_TRUE(cat.remove("y.txt"));
    EXPECT_FALSE(cat.remove("y.txt"));   // second remove is a no-op
    EXPECT_EQ(cat.size(), 0u);
}

TEST(Config, SaveLoadRoundTrip) {
    const auto dir = make_temp_dir("roundtrip");
    const std::string f1 = write_file(dir / "one.txt", "123456789");
    const std::string f2 =
        write_file(dir / "two.txt", "The quick brown fox jumps over the lazy dog");

    Catalog cat;
    ASSERT_TRUE(cat.add(f1).ok);
    ASSERT_TRUE(cat.add(f2, std::string("fox")).ok);
    const std::string cfg = (dir / "config.json").string();
    cat.save(cfg);

    const Catalog loaded = Catalog::load(cfg);
    ASSERT_EQ(loaded.size(), 2u);

    const auto one = loaded.find("one.txt");
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->size_bytes, 9u);
    EXPECT_EQ(one->checksum, checksum_from_crc32(0xCBF43926u));

    const auto fox = loaded.find("fox");
    ASSERT_TRUE(fox.has_value());
    EXPECT_EQ(fox->checksum, checksum_from_crc32(0x414FA339u));
}

TEST(Config, LoadMissingFileIsEmpty) {
    const auto dir = make_temp_dir("missing");
    const Catalog cat = Catalog::load((dir / "nope.json").string());
    EXPECT_EQ(cat.size(), 0u);
}

TEST(Config, LoadMalformedJsonThrows) {
    const auto dir = make_temp_dir("malformed");
    const std::string cfg = write_file(dir / "config.json", "{ this is not json ");
    EXPECT_THROW((void)Catalog::load(cfg), ConfigError);
}

TEST(Config, LoadUnsupportedVersionThrows) {
    const auto dir = make_temp_dir("version");
    const std::string cfg =
        write_file(dir / "config.json", R"({"version": 99, "shared_files": []})");
    EXPECT_THROW((void)Catalog::load(cfg), ConfigError);
}

TEST(Config, LoadInvalidChecksumHexThrows) {
    const auto dir = make_temp_dir("badhex");
    const std::string cfg = write_file(
        dir / "config.json",
        R"({"version":1,"shared_files":[{"alias":"a","path":"p","size_bytes":1,"checksum":"zz"}]})");
    EXPECT_THROW((void)Catalog::load(cfg), ConfigError);
}

// Regression: a present-but-wrong-typed "version" must surface as ConfigError,
// not a raw nlohmann::json::type_error (found by the M0 adversarial review).
TEST(Config, LoadNonIntegerVersionThrowsConfigError) {
    const auto dir = make_temp_dir("strversion");
    const std::string cfg =
        write_file(dir / "config.json", R"({"version":"1","shared_files":[]})");
    EXPECT_THROW((void)Catalog::load(cfg), ConfigError);
}

TEST(Config, LoadMissingVersionThrowsConfigError) {
    const auto dir = make_temp_dir("noversion");
    const std::string cfg = write_file(dir / "config.json", R"({"shared_files":[]})");
    EXPECT_THROW((void)Catalog::load(cfg), ConfigError);
}

TEST(Config, LoadEntryWrongFieldTypeThrowsConfigError) {
    const auto dir = make_temp_dir("badentry");
    const std::string cfg = write_file(
        dir / "config.json",
        R"({"version":1,"shared_files":[{"alias":"a","path":"p","size_bytes":"nope","checksum":"00"}]})");
    EXPECT_THROW((void)Catalog::load(cfg), ConfigError);
}
