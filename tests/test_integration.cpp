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

fs::path itemp(const std::string& name) {
    const fs::path dir = fs::path(::testing::TempDir()) / ("fs_it_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// Deterministic, index-dependent bytes so a reordered/duplicated chunk is caught.
std::string make_payload(std::size_t n) {
    std::string s;
    s.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>((i * 31 + 7) & 0xFF);
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

} // namespace

TEST(Integration, ListAndDownloadEndToEnd) {
    const auto dir = itemp("e2e");
    // Larger than CHUNK_SIZE (64 KiB) and not a multiple of it, to exercise
    // multi-chunk streaming with a short final chunk.
    const std::string content = make_payload(150 * 1024 + 123);
    const std::string src = write_file(dir / "sample.bin", content);

    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("sample")).ok);

    Server server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    ASSERT_NE(port, 0);
    std::thread worker([&] { server.serve_forever(); });

    Client client;
    ASSERT_NO_THROW(client.connect("127.0.0.1", port));

    const auto list = client.request_list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].alias, "sample");
    EXPECT_EQ(list[0].size, content.size());

    const std::string out = (dir / "out.bin").string();
    std::uint64_t last_received = 0;
    const auto res = client.download(
        "sample", out, list[0].size,
        [&](std::uint64_t received, std::uint64_t /*total*/) { last_received = received; });

    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok) << res.error;
    EXPECT_EQ(res.bytes, content.size());
    EXPECT_EQ(last_received, content.size());
    EXPECT_EQ(read_file(out), content); // byte-exact transfer

    client.disconnect();
    server.stop();
    worker.join();

    EXPECT_EQ(server.completed_downloads(), 1u);
    EXPECT_EQ(server.bytes_sent(), content.size());
}

TEST(Integration, DownloadUnknownAliasReturnsError) {
    Catalog catalog; // empty
    Server server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread worker([&] { server.serve_forever(); });

    Client client;
    client.connect("127.0.0.1", port);
    const auto dir = itemp("noalias");
    const auto res = client.download("ghost", (dir / "x.bin").string());

    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("ghost"), std::string::npos);

    client.disconnect();
    server.stop();
    worker.join();
}

TEST(Integration, FailedDownloadDoesNotClobberDestination) {
    Catalog catalog; // empty -> every alias is FILE_NOT_FOUND
    Server server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread worker([&] { server.serve_forever(); });

    Client client;
    client.connect("127.0.0.1", port);

    const auto dir = itemp("noclobber");
    const auto dest = dir / "important.txt";
    write_file(dest, "PRE-EXISTING DATA");

    const auto res = client.download("ghost", dest.string());
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(read_file(dest), "PRE-EXISTING DATA");                 // untouched
    EXPECT_FALSE(fs::exists(dest.string() + ".part"));              // temp cleaned up

    client.disconnect();
    server.stop();
    worker.join();
}

TEST(Integration, TwoSequentialClients) {
    const auto dir = itemp("twoclients");
    const std::string content = make_payload(70 * 1024); // just over one chunk
    const std::string src = write_file(dir / "f.bin", content);

    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("f")).ok);

    Server server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread worker([&] { server.serve_forever(); });

    for (int i = 0; i < 2; ++i) {
        Client client;
        client.connect("127.0.0.1", port);
        const auto res = client.download("f", (dir / ("o" + std::to_string(i) + ".bin")).string());
        EXPECT_TRUE(res.ok) << res.error;
        EXPECT_TRUE(res.checksum_ok);
        client.disconnect();
    }

    server.stop();
    worker.join();
    EXPECT_EQ(server.completed_downloads(), 2u);
}
