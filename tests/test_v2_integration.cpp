#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "fileshare/net.hpp"
#include "fileshare/v2/client.hpp"
#include "fileshare/v2/server.hpp"
#include "fileshare/v2/server_context.hpp"
#include "fileshare/v2/settings.hpp"

namespace fs = std::filesystem;
using namespace fileshare::v2;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

class V2Server : public ::testing::Test {
protected:
    void SetUp() override {
        fileshare::net::startup();
        static std::atomic<int> counter{0};
        base_ = fs::temp_directory_path() /
                ("fileshare_v2_it_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        share_ = base_ / "share";
        out_ = base_ / "out";
        fs::create_directories(share_ / "video");
        fs::create_directories(out_);
        std::ofstream(share_ / "readme.txt", std::ios::binary) << "hello world";
        std::ofstream(share_ / "video" / "clip.bin", std::ios::binary) << big_content_;

        Settings s;
        s.port = 0;
        s.share_root = share_.string();
        s.checksum_cache_file = "";                 // no persistence during tests
        s.limits.handshake_timeout_s = 30;
        s.limits.idle_timeout_s = 30;
        ctx_ = std::make_unique<ServerContext>(s, "");
        server_ = std::make_unique<Server>(*ctx_);
        port_ = server_->bind(0);
        thread_ = std::thread([this] { server_->serve(/*grace=*/2); });
    }

    void TearDown() override {
        server_->stop();
        if (thread_.joinable()) thread_.join();
        server_.reset();
        ctx_.reset();
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    void connect_ok(Client& c, const std::string& login = "vit") {
        const auto r = c.connect("127.0.0.1", port_, login, "");
        ASSERT_TRUE(r.ok) << r.error;
    }

    std::string big_content_ = std::string(200u * 1024 + 123, 'Z');  // multi-chunk
    fs::path base_, share_, out_;
    std::uint16_t port_ = 0;
    std::unique_ptr<ServerContext> ctx_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

// --- Handshake --------------------------------------------------------------
TEST_F(V2Server, ConnectGrantsAdminInNoAuthMode) {
    Client c;
    const auto r = c.connect("127.0.0.1", port_, "vit", "");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.role, Role::ADMIN);       // no-auth bootstrap
    EXPECT_NE(r.session_id, 0u);
}

// --- Browsing ---------------------------------------------------------------
TEST_F(V2Server, ListRoot) {
    Client c; connect_ok(c);
    const auto entries = c.list_dir("/");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "video");
    EXPECT_EQ(entries[0].kind, EntryKind::DIR);
    EXPECT_EQ(entries[1].name, "readme.txt");
    EXPECT_EQ(entries[1].size, 11u);
}

TEST_F(V2Server, ListSubdirAndStat) {
    Client c; connect_ok(c);
    const auto entries = c.list_dir("/video");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "clip.bin");

    const DirEntry st = c.stat("/video/clip.bin");
    EXPECT_EQ(st.size, big_content_.size());
    EXPECT_EQ(st.kind, EntryKind::FILE);
}

TEST_F(V2Server, ListMissingDirReturnsError) {
    Client c; connect_ok(c);
    try {
        (void)c.list_dir("/nope");
        FAIL() << "expected RemoteError";
    } catch (const RemoteError& e) {
        EXPECT_EQ(e.code(), ErrCode::FILE_NOT_FOUND);
    }
}

TEST_F(V2Server, TraversalRejected) {
    Client c; connect_ok(c);
    try {
        (void)c.list_dir("/../../etc");
        FAIL() << "expected RemoteError";
    } catch (const RemoteError& e) {
        EXPECT_EQ(e.code(), ErrCode::BAD_REQUEST);
    }
}

// --- Download ---------------------------------------------------------------
TEST_F(V2Server, DownloadSmallFileVerified) {
    Client c; connect_ok(c);
    const fs::path dst = out_ / "readme.txt";
    const auto r = c.download("/readme.txt", dst.string());
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.checksum_ok);
    EXPECT_EQ(read_file(dst), "hello world");
    EXPECT_FALSE(fs::exists(dst.string() + ".part"));   // .part renamed away
}

TEST_F(V2Server, DownloadMultiChunkVerified) {
    Client c; connect_ok(c);
    const fs::path dst = out_ / "clip.bin";
    std::uint64_t last_done = 0, last_total = 0;
    const auto r = c.download("/video/clip.bin", dst.string(),
                              [&](std::uint64_t d, std::uint64_t t) { last_done = d; last_total = t; });
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.checksum_ok);
    EXPECT_EQ(fs::file_size(dst), big_content_.size());
    EXPECT_EQ(read_file(dst), big_content_);
    EXPECT_EQ(last_done, big_content_.size());
    EXPECT_EQ(last_total, big_content_.size());
}

TEST_F(V2Server, DownloadResumesFromPart) {
    Client c; connect_ok(c);
    const fs::path dst = out_ / "clip.bin";
    const fs::path part = fs::path(dst.string() + ".part");
    // Pre-seed a .part with the correct first 50000 bytes, as if interrupted.
    const std::size_t pre = 50000;
    { std::ofstream(part, std::ios::binary) << big_content_.substr(0, pre); }

    const auto r = c.download("/video/clip.bin", dst.string());
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.resumed);
    EXPECT_TRUE(r.checksum_ok);
    EXPECT_EQ(read_file(dst), big_content_);   // resumed bytes + streamed tail = whole file
}

TEST_F(V2Server, DownloadMissingFileErrors) {
    Client c; connect_ok(c);
    const auto r = c.download("/nope.bin", (out_ / "nope.bin").string());
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST_F(V2Server, DownloadDirectoryRejected) {
    Client c; connect_ok(c);
    const auto r = c.download("/video", (out_ / "video").string());
    EXPECT_FALSE(r.ok);
}

// --- Admin ------------------------------------------------------------------
TEST_F(V2Server, AdminStatsAndClients) {
    Client c; connect_ok(c);
    const AdminStats st = c.admin_stats();
    EXPECT_EQ(st.version, std::string(SERVER_VERSION));
    EXPECT_GE(st.active_connections, 1u);

    const auto clients = c.admin_list_clients();
    ASSERT_GE(clients.size(), 1u);
    bool found = false;
    for (const auto& cl : clients) {
        if (cl.login == "vit") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(V2Server, AdminKickSelfRefused) {
    Client c; connect_ok(c);
    const AdminStats st = c.admin_stats();
    (void)st;
    const auto clients = c.admin_list_clients();
    ASSERT_GE(clients.size(), 1u);
    const auto res = c.admin_kick(clients.front().session_id);
    EXPECT_FALSE(res.ok);   // that's our own session
}

TEST_F(V2Server, AdminGetConfigReturnsJson) {
    Client c; connect_ok(c);
    const std::string cfg = c.admin_get_config();
    EXPECT_NE(cfg.find("share_root"), std::string::npos);
}

// --- Multiple clients -------------------------------------------------------
TEST_F(V2Server, TwoClientsConcurrent) {
    Client a, b;
    connect_ok(a, "alice");
    connect_ok(b, "bob");
    EXPECT_EQ(a.list_dir("/").size(), 2u);
    EXPECT_EQ(b.list_dir("/video").size(), 1u);
    const auto ra = a.download("/readme.txt", (out_ / "a.txt").string());
    const auto rb = b.download("/readme.txt", (out_ / "b.txt").string());
    EXPECT_TRUE(ra.ok);
    EXPECT_TRUE(rb.ok);
}
