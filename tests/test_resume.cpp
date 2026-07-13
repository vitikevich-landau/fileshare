#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "fileshare/client.hpp"
#include "fileshare/config.hpp"
#include "fileshare/server.hpp"

using namespace fileshare;
namespace fs = std::filesystem;

namespace {

fs::path rtemp(const std::string& name) {
    const fs::path dir = fs::path(::testing::TempDir()) / ("fs_rs_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string make_payload(std::size_t n) {
    std::string s;
    s.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>((i * 131 + 17) & 0xFF);
    }
    return s;
}

std::string write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
    out.close();
    return p.string();
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A server sharing one "blob" running on its own thread.
struct ResumeServer {
    Server        server;
    std::uint16_t port;
    std::thread   engine;

    explicit ResumeServer(Catalog cat) : server(std::move(cat)) {
        port = server.listen(0);
        engine = std::thread([this] { server.serve_forever(); });
    }
    ~ResumeServer() {
        server.stop();
        engine.join();
    }
};

Catalog blob_catalog(const fs::path& dir, const std::string& content) {
    Catalog cat;
    cat.add(write_file(dir / "blob.bin", content), std::string("blob"));
    return cat;
}

} // namespace

TEST(Resume, CompletesFromPartialPart) {
    const auto dir = rtemp("partial");
    const std::string content = make_payload(250 * 1024 + 55);
    ResumeServer srv(blob_catalog(dir, content));

    const fs::path out = dir / "out.bin";
    const fs::path part = dir / "out.bin.part";
    write_file(part, content.substr(0, 100 * 1024)); // simulate an interrupted download

    Client client;
    client.connect("127.0.0.1", srv.port);
    const auto res = client.download("blob", out.string(), content.size());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok);
    EXPECT_EQ(res.bytes, content.size());        // total counts the resumed bytes too
    EXPECT_EQ(read_file(out), content);          // byte-exact whole file
    EXPECT_FALSE(fs::exists(part));               // renamed into place
    client.disconnect();
}

TEST(Resume, CompletePartFinishesWithNoNewBytes) {
    const auto dir = rtemp("full");
    const std::string content = make_payload(80 * 1024);
    ResumeServer srv(blob_catalog(dir, content));

    const fs::path out = dir / "out.bin";
    write_file(dir / "out.bin.part", content); // already fully downloaded, just never renamed

    Client client;
    client.connect("127.0.0.1", srv.port);
    const auto res = client.download("blob", out.string(), content.size());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok);
    EXPECT_EQ(read_file(out), content);
    client.disconnect();
}

TEST(Resume, CorruptPartFailsAndIsRemoved) {
    const auto dir = rtemp("corrupt");
    const std::string content = make_payload(200 * 1024);
    ResumeServer srv(blob_catalog(dir, content));

    const fs::path out = dir / "out.bin";
    const fs::path part = dir / "out.bin.part";
    // A partial whose bytes do NOT match the file's prefix.
    write_file(part, std::string(90 * 1024, '\xAB'));

    Client client;
    client.connect("127.0.0.1", srv.port);
    const auto res = client.download("blob", out.string(), content.size());
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("checksum"), std::string::npos);
    EXPECT_FALSE(fs::exists(out));       // destination untouched
    EXPECT_FALSE(fs::exists(part));      // corrupt partial cleared for a fresh retry
    client.disconnect();
}

TEST(Resume, FreshDownloadLeavesNoPart) {
    const auto dir = rtemp("fresh");
    const std::string content = make_payload(120 * 1024);
    ResumeServer srv(blob_catalog(dir, content));

    const fs::path out = dir / "out.bin";
    Client client;
    client.connect("127.0.0.1", srv.port);
    const auto res = client.download("blob", out.string(), content.size());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_EQ(read_file(out), content);
    EXPECT_FALSE(fs::exists(dir / "out.bin.part"));
    client.disconnect();
}
