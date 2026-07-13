#include <gtest/gtest.h>

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
#include "fileshare/server.hpp"

using namespace fileshare;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path ctemp(const std::string& name) {
    const fs::path dir = fs::path(::testing::TempDir()) / ("fs_ct_" + name);
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

// Poll until `pred` holds or the deadline passes.
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

TEST(Concurrency, ManyClientsDownloadSimultaneously) {
    const auto dir = ctemp("many");
    const std::string content = make_payload(200 * 1024 + 77); // several chunks
    const std::string src = write_file(dir / "blob.bin", content);

    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("blob")).ok);

    Server server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    constexpr int kClients = 8;
    std::vector<std::thread> threads;
    std::atomic<int> ok_count{0};
    std::atomic<int> checksum_ok_count{0};

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            try {
                Client client;
                client.connect("127.0.0.1", port);
                const std::string out = (dir / ("out_" + std::to_string(i) + ".bin")).string();
                const auto res = client.download("blob", out);
                if (res.ok) {
                    ok_count.fetch_add(1);
                }
                if (res.checksum_ok && read_file(out) == content) {
                    checksum_ok_count.fetch_add(1);
                }
                client.disconnect();
            } catch (...) {
                // leave the counters unincremented -> test fails below
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(ok_count.load(), kClients);
    EXPECT_EQ(checksum_ok_count.load(), kClients);

    server.stop();
    engine.join();
    EXPECT_EQ(server.completed_downloads(), static_cast<std::uint64_t>(kClients));
    EXPECT_EQ(server.bytes_sent(), static_cast<std::uint64_t>(kClients) * content.size());
}

TEST(Concurrency, KickDisconnectsClient) {
    Catalog catalog; // empty is fine; we only test the control channel
    Server server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client client;
    client.connect("127.0.0.1", port);
    ASSERT_TRUE(wait_until([&] { return server.client_count() == 1; }));

    const auto clients = server.admin_list_clients();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_TRUE(server.admin_kick(clients[0].id));

    // After the kick the server half-closed the socket; the next request fails.
    EXPECT_ANY_THROW((void)client.request_list());
    EXPECT_TRUE(wait_until([&] { return server.client_count() == 0; }));
    EXPECT_FALSE(server.admin_kick(clients[0].id)); // gone now

    client.disconnect();
    server.stop();
    engine.join();
}

TEST(Concurrency, AdminAddRemoveListAndStatus) {
    const auto dir = ctemp("admin");
    const std::string f1 = write_file(dir / "a.bin", make_payload(1000));
    const std::string cfg = (dir / "config.json").string();

    Server server(Catalog{}, cfg);
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    EXPECT_TRUE(server.admin_list_files().empty());

    const auto added = server.admin_add(f1, std::string("a"));
    ASSERT_TRUE(added.ok) << added.error;
    ASSERT_EQ(server.admin_list_files().size(), 1u);
    EXPECT_EQ(server.admin_list_files()[0].alias, "a");

    // Persisted to config.json and reloadable.
    const Catalog reloaded = Catalog::load(cfg);
    EXPECT_EQ(reloaded.size(), 1u);

    const auto status = server.admin_status();
    EXPECT_EQ(status.shared_files, 1u);
    EXPECT_EQ(status.active_connections, 0u);

    EXPECT_TRUE(server.admin_remove("a"));
    EXPECT_FALSE(server.admin_remove("a"));
    EXPECT_TRUE(server.admin_list_files().empty());

    server.stop();
    engine.join();
}

TEST(Concurrency, BadAdminCommandDoesNotKillEngine) {
    const auto dir = ctemp("badcmd");
    const std::string cfg = (dir / "config.json").string();

    Server server(Catalog{}, cfg);
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    // `add` with a non-UTF-8 alias: add() accepts it, save() throws ConfigError
    // (handled), plus some outright garbage. All routed through the queue -> the
    // engine must survive and keep serving (no escape past teardown).
    const std::string f = write_file(dir / "x.bin", make_payload(1000));
    server.submit_command(std::string("add ") + f + " \xff\xfe");
    server.submit_command("garbage-command-xyz");
    server.submit_command("status");

    // Proof the engine is still alive and accepting: a client connects + lists.
    Client client;
    ASSERT_NO_THROW(client.connect("127.0.0.1", port));
    ASSERT_NO_THROW((void)client.request_list());

    client.disconnect();
    server.stop();
    engine.join(); // returns only if the engine thread is healthy
}

TEST(Concurrency, ClientsSnapshotAndSubmitCommandDoNotCrash) {
    const auto dir = ctemp("cmds");
    const std::string src = write_file(dir / "x.bin", make_payload(50 * 1024));
    Catalog catalog;
    ASSERT_TRUE(catalog.add(src, std::string("x")).ok);

    Server server(std::move(catalog));
    const std::uint16_t port = server.listen(0);
    std::thread engine([&] { server.serve_forever(); });

    Client client;
    client.connect("127.0.0.1", port);
    ASSERT_TRUE(wait_until([&] { return server.client_count() == 1; }));

    // These go through the command queue -> engine (exercises drain_commands).
    server.submit_command("status");
    server.submit_command("clients");
    server.submit_command("list");

    const auto snap = server.admin_list_clients();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_NE(snap[0].peer.find("127.0.0.1"), std::string::npos);

    client.disconnect();
    server.stop();
    engine.join();
}
