#include <gtest/gtest.h>

#ifdef FILESHARE_HAVE_EPOLL

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "fileshare/client.hpp"
#include "fileshare/config.hpp"
#include "fileshare/epoll_server.hpp"

using namespace fileshare;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path etemp(const std::string& name) {
    const fs::path dir = fs::path(::testing::TempDir()) / ("fs_ep_" + name);
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
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

} // namespace

TEST(Epoll, ListAndDownloadEndToEnd) {
    const auto dir = etemp("e2e");
    const std::string content = make_payload(200 * 1024 + 33);
    const std::string src = write_file(dir / "sample.bin", content);

    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("sample")).ok);

    EpollServer server(std::move(catalog), {}, 2);
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client client;
    ASSERT_NO_THROW(client.connect("127.0.0.1", port));
    const auto list = client.request_list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].alias, "sample");
    EXPECT_EQ(list[0].size, content.size());

    const std::string out = (dir / "out.bin").string();
    const auto res = client.download("sample", out, list[0].size);
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok) << res.error;
    EXPECT_EQ(read_file(out), content);

    client.disconnect();
    server.stop();
    engine.join();
    EXPECT_EQ(server.completed_downloads(), 1u);
}

// A file well beyond the socket buffer forces send() to hit EWOULDBLOCK, so the
// download must resume across multiple EPOLLOUT events -- exercising the
// backpressure state machine.
TEST(Epoll, LargeDownloadWithBackpressure) {
    const auto dir = etemp("big");
    const std::string content = make_payload(8 * 1024 * 1024 + 777);
    const std::string src = write_file(dir / "big.bin", content);

    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("big")).ok);

    EpollServer server(std::move(catalog), {}, 2);
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client client;
    client.connect("127.0.0.1", port);
    const std::string out = (dir / "big_out.bin").string();
    const auto res = client.download("big", out, content.size());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok) << res.error;
    EXPECT_EQ(res.bytes, content.size());
    EXPECT_EQ(read_file(out), content);

    client.disconnect();
    server.stop();
    engine.join();
}

TEST(Epoll, ManyClientsConcurrent) {
    const auto dir = etemp("many");
    const std::string content = make_payload(300 * 1024 + 91);
    const std::string src = write_file(dir / "blob.bin", content);

    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("blob")).ok);

    EpollServer server(std::move(catalog), {}, 4);
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    constexpr int kClients = 12;
    std::vector<std::thread> threads;
    std::atomic<int> ok_count{0};
    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            try {
                Client client;
                client.connect("127.0.0.1", port);
                const std::string out = (dir / ("o" + std::to_string(i) + ".bin")).string();
                const auto res = client.download("blob", out);
                if (res.ok && res.checksum_ok && read_file(out) == content) {
                    ok_count.fetch_add(1);
                }
                client.disconnect();
            } catch (...) {
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(ok_count.load(), kClients);

    server.stop();
    engine.join();
    EXPECT_EQ(server.completed_downloads(), static_cast<std::uint64_t>(kClients));
}

TEST(Epoll, KickDisconnectsClient) {
    Catalog catalog;
    EpollServer server(std::move(catalog), {}, 2);
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client client;
    client.connect("127.0.0.1", port);
    ASSERT_TRUE(wait_until([&] { return server.client_count() == 1; }));
    const auto clients = server.admin_list_clients();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_TRUE(server.admin_kick(clients[0].id));

    EXPECT_ANY_THROW((void)client.request_list());
    EXPECT_TRUE(wait_until([&] { return server.client_count() == 0; }));

    client.disconnect();
    server.stop();
    engine.join();
}

TEST(Epoll, GracefulShutdownLetsActiveDownloadFinish) {
    const auto dir = etemp("graceful");
    const std::string content = make_payload(12 * 1024 * 1024);
    const std::string src = write_file(dir / "big.bin", content);
    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("big")).ok);

    EpollServer server(std::move(catalog), {}, 2, std::chrono::seconds(5));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client::DownloadResult result;
    const std::string out = (dir / "out.bin").string();
    std::thread dl([&] {
        Client c;
        c.connect("127.0.0.1", port);
        result = c.download("big", out, content.size());
        c.disconnect();
    });

    ASSERT_TRUE(wait_until([&] { return server.downloads_in_progress() == 1; }));
    server.stop(); // graceful drain must let the in-flight download finish

    dl.join();
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.checksum_ok);
    EXPECT_EQ(read_file(out), content);
    engine.join();
}

TEST(Epoll, HostileFrameDropsOnlyThatConnection) {
    const auto dir = etemp("hostile");
    const std::string content = make_payload(64 * 1024);
    const std::string src = write_file(dir / "blob.bin", content);
    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("blob")).ok);

    EpollServer server(std::move(catalog), {}, 2);
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    net::Socket hostile = net::tcp_connect("127.0.0.1", port);
    net::send_all(hostile, std::vector<std::uint8_t>{0xFF, 0, 0, 0, 0}); // unknown type
    const std::optional<Frame> reply = net::recv_message(hostile);
    EXPECT_FALSE(reply.has_value());

    Client good;
    good.connect("127.0.0.1", port);
    const auto res = good.download("blob", (dir / "out.bin").string());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok);
    good.disconnect();

    server.stop();
    engine.join();
}

TEST(Epoll, UnknownAliasReturnsError) {
    Catalog catalog;
    EpollServer server(std::move(catalog), {}, 2);
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client client;
    client.connect("127.0.0.1", port);
    const auto dir = etemp("noalias");
    const auto res = client.download("ghost", (dir / "x.bin").string());
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("ghost"), std::string::npos);

    client.disconnect();
    server.stop();
    engine.join();
}

#endif // FILESHARE_HAVE_EPOLL
