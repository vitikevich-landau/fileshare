#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "fileshare/net.hpp"
#include "fileshare/v2/auth.hpp"
#include "fileshare/v2/client.hpp"
#include "fileshare/v2/server.hpp"
#include "fileshare/v2/server_context.hpp"
#include "fileshare/v2/settings.hpp"

namespace fs = std::filesystem;
using namespace fileshare::v2;

namespace { constexpr std::uint32_t T_ITERS = 2048; }

// Server with a real user database (challenge auth enabled).
class V2AuthServer : public ::testing::Test {
protected:
    void SetUp() override {
        fileshare::net::startup();
        static std::atomic<int> counter{0};
        base_ = fs::temp_directory_path() /
                ("fileshare_v2_auth_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        fs::create_directories(base_ / "share");
        std::ofstream(base_ / "share" / "readme.txt", std::ios::binary) << "hi";

        const std::string users_file = (base_ / "users.json").string();
        {
            UserDb db;
            db.set(make_user("admin", Role::ADMIN, "adminpw", T_ITERS));
            db.set(make_user("alice", Role::USER, "alicepw", T_ITERS));
            db.save(users_file);
        }

        Settings s;
        s.port = 0;
        s.share_root = (base_ / "share").string();
        s.users_file = users_file;
        s.auth_pbkdf2_iters = T_ITERS;
        s.checksum_cache_file = "";
        s.limits.handshake_timeout_s = 30;
        s.limits.idle_timeout_s = 30;
        s.limits.auth_fail_ban_s = 60;
        s.limits.max_sessions_per_user = 2;
        ctx_ = std::make_unique<ServerContext>(s, "");
        server_ = std::make_unique<Server>(*ctx_);
        port_ = server_->bind(0);
        thread_ = std::thread([this] { server_->serve(2); });
    }

    void TearDown() override {
        server_->stop();
        if (thread_.joinable()) thread_.join();
        server_.reset();
        ctx_.reset();
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    fs::path base_;
    std::uint16_t port_ = 0;
    std::unique_ptr<ServerContext> ctx_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

TEST_F(V2AuthServer, CorrectAdminLogin) {
    Client c;
    const auto r = c.connect("127.0.0.1", port_, "admin", "adminpw");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.role, Role::ADMIN);
}

TEST_F(V2AuthServer, CorrectUserLogin) {
    Client c;
    const auto r = c.connect("127.0.0.1", port_, "alice", "alicepw");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.role, Role::USER);
    EXPECT_EQ(c.list_dir("/").size(), 1u);   // a user can browse
}

TEST_F(V2AuthServer, WrongPasswordRejected) {
    Client c;
    const auto r = c.connect("127.0.0.1", port_, "admin", "wrongpw");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, ErrCode::AUTH_FAILED);
}

TEST_F(V2AuthServer, UnknownUserRejected) {
    Client c;
    const auto r = c.connect("127.0.0.1", port_, "nobody", "x");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, ErrCode::AUTH_FAILED);
}

TEST_F(V2AuthServer, UserCannotUseAdminChannel) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "alice", "alicepw").ok);
    try {
        (void)c.admin_stats();
        FAIL() << "expected ACCESS_DENIED";
    } catch (const RemoteError& e) {
        EXPECT_EQ(e.code(), ErrCode::ACCESS_DENIED);
    }
}

TEST_F(V2AuthServer, AdminCanUseAdminChannel) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "admin", "adminpw").ok);
    const AdminStats st = c.admin_stats();
    EXPECT_GE(st.active_connections, 1u);
}

TEST_F(V2AuthServer, BanAfterRepeatedFailures) {
    // Three wrong attempts from this IP, then even a correct one is refused.
    for (int i = 0; i < 3; ++i) {
        Client c;
        const auto r = c.connect("127.0.0.1", port_, "admin", "wrongpw");
        EXPECT_FALSE(r.ok);
        EXPECT_EQ(r.error_code, ErrCode::AUTH_FAILED);
    }
    Client c;
    const auto r = c.connect("127.0.0.1", port_, "admin", "adminpw");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, ErrCode::RATE_LIMITED);
}

TEST_F(V2AuthServer, MaxSessionsPerUserEnforced) {
    Client a, b, c;
    ASSERT_TRUE(a.connect("127.0.0.1", port_, "alice", "alicepw").ok);
    ASSERT_TRUE(b.connect("127.0.0.1", port_, "alice", "alicepw").ok);
    // max_sessions_per_user = 2; the third concurrent alice is refused.
    const auto r = c.connect("127.0.0.1", port_, "alice", "alicepw");
    EXPECT_FALSE(r.ok);
}

TEST_F(V2AuthServer, DownloadWorksAfterAuth) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "alice", "alicepw").ok);
    const fs::path dst = base_ / "readme.txt";
    const auto r = c.download("/readme.txt", dst.string());
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.checksum_ok);
}
