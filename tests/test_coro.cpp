#include <gtest/gtest.h>

#ifdef FILESHARE_USE_COROUTINES

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
#include "fileshare/epoll_coro_server.hpp"
#include "fileshare/net.hpp"
#include "fileshare/protocol.hpp"

using namespace fileshare;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path ctemp(const std::string& name) {
    const fs::path dir = fs::path(::testing::TempDir()) / ("fs_co_" + name);
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

} // namespace

TEST(Coro, ListAndDownloadEndToEnd) {
    const auto dir = ctemp("e2e");
    const std::string content = make_payload(200 * 1024 + 33);
    Catalog catalog;
    ASSERT_TRUE(catalog.add(write_file(dir / "s.bin", content), std::string("s")).ok);

    CoroServer server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client client;
    ASSERT_NO_THROW(client.connect("127.0.0.1", port));
    const auto list = client.request_list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].size, content.size());
    const auto res = client.download("s", (dir / "out.bin").string(), content.size());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok);
    EXPECT_EQ(read_file(dir / "out.bin"), content);

    client.disconnect();
    server.stop();
    engine.join();
    EXPECT_EQ(server.completed_downloads(), 1u);
}

TEST(Coro, LargeDownloadWithBackpressure) {
    const auto dir = ctemp("big");
    const std::string content = make_payload(8 * 1024 * 1024 + 777);
    Catalog catalog;
    ASSERT_TRUE(catalog.add(write_file(dir / "b.bin", content), std::string("b")).ok);

    CoroServer server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client client;
    client.connect("127.0.0.1", port);
    const auto res = client.download("b", (dir / "out.bin").string(), content.size());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok);
    EXPECT_EQ(read_file(dir / "out.bin"), content);

    client.disconnect();
    server.stop();
    engine.join();
}

TEST(Coro, ManyClientsConcurrent) {
    const auto dir = ctemp("many");
    const std::string content = make_payload(300 * 1024 + 5);
    Catalog catalog;
    ASSERT_TRUE(catalog.add(write_file(dir / "m.bin", content), std::string("m")).ok);

    CoroServer server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    constexpr int kClients = 10;
    std::vector<std::thread> threads;
    std::atomic<int> ok_count{0};
    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            try {
                Client client;
                client.connect("127.0.0.1", port);
                const std::string out = (dir / ("o" + std::to_string(i) + ".bin")).string();
                const auto res = client.download("m", out);
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

TEST(Coro, KickDisconnectsClient) {
    Catalog catalog;
    CoroServer server(std::move(catalog));
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

TEST(Coro, HostileFrameDropsOnlyThatConnection) {
    const auto dir = ctemp("hostile");
    const std::string content = make_payload(64 * 1024);
    Catalog catalog;
    ASSERT_TRUE(catalog.add(write_file(dir / "h.bin", content), std::string("h")).ok);

    CoroServer server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    net::Socket hostile = net::tcp_connect("127.0.0.1", port);
    net::send_all(hostile, std::vector<std::uint8_t>{0xFF, 0, 0, 0, 0}); // unknown type
    EXPECT_FALSE(net::recv_message(hostile).has_value());

    Client good;
    good.connect("127.0.0.1", port);
    const auto res = good.download("h", (dir / "out.bin").string());
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.checksum_ok);
    good.disconnect();

    server.stop();
    engine.join();
}

TEST(Coro, GracefulShutdownLetsActiveDownloadFinish) {
    const auto dir = ctemp("graceful");
    const std::string content = make_payload(12 * 1024 * 1024);
    Catalog catalog;
    ASSERT_TRUE(catalog.add(write_file(dir / "g.bin", content), std::string("g")).ok);

    CoroServer server(std::move(catalog), {}, std::chrono::seconds(5));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client::DownloadResult result;
    const std::string out = (dir / "out.bin").string();
    std::thread dl([&] {
        Client c;
        c.connect("127.0.0.1", port);
        result = c.download("g", out, content.size());
        c.disconnect();
    });
    ASSERT_TRUE(wait_until([&] { return server.downloads_in_progress() == 1; }));
    server.stop();

    dl.join();
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.checksum_ok);
    EXPECT_EQ(read_file(out), content);
    engine.join();
}

#endif // FILESHARE_USE_COROUTINES
