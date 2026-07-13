#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "fileshare/client.hpp"
#include "fileshare/config.hpp"
#include "fileshare/net.hpp"
#include "fileshare/protocol.hpp"
#include "fileshare/server.hpp"

using namespace fileshare;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path ftemp(const std::string& name) {
    const fs::path dir = fs::path(::testing::TempDir()) / ("fs_ft_" + name);
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

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

// A server sharing one file, running on its own thread.
struct Fixture {
    fs::path      dir;
    std::string   content;
    Server        server;
    std::uint16_t port;
    std::thread   engine;

    Fixture(const std::string& name, std::size_t size,
            std::chrono::milliseconds grace = std::chrono::seconds(5))
        : dir(ftemp(name)),
          content(make_payload(size)),
          server(make_catalog(dir, content), {}, grace) {
        port = server.listen(0);
        engine = std::thread([this] { server.serve_forever(); });
    }
    ~Fixture() {
        server.stop();
        if (engine.joinable()) {
            engine.join();
        }
    }
    static Catalog make_catalog(const fs::path& d, const std::string& content) {
        Catalog cat;
        const std::string src = write_file(d / "blob.bin", content);
        cat.add(src, std::string("blob"));
        return cat;
    }
};

} // namespace

TEST(Faults, HostileUnknownTypeFrameDropsOnlyThatConnection) {
    Fixture fx("hostile_type", 64 * 1024);

    // Raw hostile client: a frame with an unknown message type.
    net::Socket hostile = net::tcp_connect("127.0.0.1", fx.port);
    net::send_all(hostile, std::vector<std::uint8_t>{0xFF, 0, 0, 0, 0});
    const std::optional<Frame> reply = net::recv_message(hostile); // server drops us
    EXPECT_FALSE(reply.has_value());

    // A well-behaved client still works afterwards.
    Client good;
    good.connect("127.0.0.1", fx.port);
    const auto res = good.download("blob", (fx.dir / "out.bin").string());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok);
    good.disconnect();
}

TEST(Faults, OversizePayloadHeaderDropsConnection) {
    Fixture fx("hostile_size", 64 * 1024);

    net::Socket hostile = net::tcp_connect("127.0.0.1", fx.port);
    // LIST_REQUEST header but a payload_length far above MAX_CONTROL_PAYLOAD.
    const std::uint32_t huge = MAX_CONTROL_PAYLOAD + 1;
    std::vector<std::uint8_t> frame{static_cast<std::uint8_t>(MessageType::LIST_REQUEST),
                                    static_cast<std::uint8_t>((huge >> 24) & 0xFF),
                                    static_cast<std::uint8_t>((huge >> 16) & 0xFF),
                                    static_cast<std::uint8_t>((huge >> 8) & 0xFF),
                                    static_cast<std::uint8_t>(huge & 0xFF)};
    net::send_all(hostile, frame);
    const std::optional<Frame> reply = net::recv_message(hostile);
    EXPECT_FALSE(reply.has_value());

    Client good;
    good.connect("127.0.0.1", fx.port);
    EXPECT_NO_THROW((void)good.request_list());
    good.disconnect();
}

TEST(Faults, MidDownloadAbruptCloseServerSurvives) {
    Fixture fx("abrupt", 4 * 1024 * 1024);

    {
        // Start a download and drop the socket after the first chunk.
        net::Socket s = net::tcp_connect("127.0.0.1", fx.port);
        net::send_all(s, encode_download_request(DownloadRequest{"blob", 0}));
        const std::optional<Frame> first = net::recv_message(s);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(first->type, MessageType::CHUNK_DATA);
        s.close(); // abrupt disconnect mid-transfer
    }

    // The server must still be healthy for the next client.
    Client good;
    good.connect("127.0.0.1", fx.port);
    const auto res = good.download("blob", (fx.dir / "out.bin").string());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok);
    EXPECT_EQ(read_file(fx.dir / "out.bin"), fx.content);
    good.disconnect();
}

TEST(Faults, RemoveDuringDownloadStillCompletes) {
    Fixture fx("remove_mid", 12 * 1024 * 1024);

    Client::DownloadResult result;
    const std::string out = (fx.dir / "out.bin").string();
    std::thread dl([&] {
        Client c;
        c.connect("127.0.0.1", fx.port);
        result = c.download("blob", out, fx.content.size());
        c.disconnect();
    });

    // Remove the alias while the transfer is in flight (§7: an already-open file
    // keeps streaming to completion).
    ASSERT_TRUE(wait_until([&] { return fx.server.downloads_in_progress() == 1; }));
    EXPECT_TRUE(fx.server.admin_remove("blob"));
    EXPECT_TRUE(fx.server.admin_list_files().empty());

    dl.join();
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.checksum_ok);
    EXPECT_EQ(read_file(out), fx.content);
}

TEST(Faults, GracefulShutdownLetsActiveDownloadFinish) {
    Fixture fx("graceful", 12 * 1024 * 1024);

    Client::DownloadResult result;
    const std::string out = (fx.dir / "out.bin").string();
    std::thread dl([&] {
        Client c;
        c.connect("127.0.0.1", fx.port);
        result = c.download("blob", out, fx.content.size());
        c.disconnect();
    });

    // Shut down mid-download: the graceful drain must let it finish, not cut it.
    ASSERT_TRUE(wait_until([&] { return fx.server.downloads_in_progress() == 1; }));
    fx.server.stop();

    dl.join();
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.checksum_ok);
    EXPECT_EQ(read_file(out), fx.content);
    // ~Fixture joins the engine (already draining/stopped).
}
