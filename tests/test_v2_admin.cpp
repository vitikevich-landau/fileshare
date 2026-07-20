#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "fileshare/net.hpp"
#include "fileshare/v2/client.hpp"
#include "fileshare/v2/protocol.hpp"
#include "fileshare/v2/rate_limiter.hpp"
#include "fileshare/v2/server.hpp"
#include "fileshare/v2/server_context.hpp"
#include "fileshare/v2/settings.hpp"
#include "fileshare/v2/settings_hub.hpp"

namespace fs = std::filesystem;
using namespace fileshare::v2;
using namespace std::chrono_literals;

// --- SettingsHub unit -------------------------------------------------------
TEST(SettingsHub, SnapshotSwap) {
    Settings init;
    init.limits.per_client_bps = 100;
    SettingsHub hub(init);
    EXPECT_EQ(hub.current()->limits.per_client_bps, 100u);

    std::string changed_key, changed_val;
    hub.set_change_cb([&](const std::string& k, const std::string& v) { changed_key = k; changed_val = v; });
    EXPECT_EQ(hub.set("limits.per_client_bps", "5000"), "");
    EXPECT_EQ(hub.current()->limits.per_client_bps, 5000u);
    EXPECT_EQ(changed_key, "limits.per_client_bps");
    EXPECT_EQ(changed_val, "5000");
}

TEST(SettingsHub, RejectsRestartKey) {
    SettingsHub hub(Settings{});
    EXPECT_FALSE(hub.set("server.port", "9999").empty());       // restart-only
    EXPECT_FALSE(hub.set("server.share_root", "/x").empty());   // restart-only
    EXPECT_FALSE(hub.set("bogus.key", "1").empty());            // unknown
}

TEST(SettingsHub, RejectsBadValueAtomically) {
    Settings init;
    init.limits.per_client_bps = 42;
    SettingsHub hub(init);
    EXPECT_FALSE(hub.set("limits.per_client_bps", "notanumber").empty());
    EXPECT_EQ(hub.current()->limits.per_client_bps, 42u);       // unchanged
    // Invalid relationship (per_client > global) is rejected by validate().
    EXPECT_EQ(hub.set("limits.global_bps", "1000"), "");
    EXPECT_FALSE(hub.set("limits.per_client_bps", "5000").empty());
    EXPECT_EQ(hub.current()->limits.per_client_bps, 42u);
}

TEST(SettingsHub, MotdAndLogLevel) {
    SettingsHub hub(Settings{});
    EXPECT_EQ(hub.set("server.motd", "hello there"), "");
    EXPECT_EQ(hub.current()->motd, "hello there");
    EXPECT_EQ(hub.set("log.level", "debug"), "");
    EXPECT_EQ(hub.current()->log_level, "debug");
    EXPECT_FALSE(hub.set("log.level", "loud").empty());
}

// --- RateLimiter unit -------------------------------------------------------
TEST(RateLimiter, UnlimitedGrantsImmediately) {
    RateLimiter rl;
    EXPECT_EQ(rl.throttle("vit", 0, 0, 65536), 65536u);
}

TEST(RateLimiter, LimitTakesProportionalTime) {
    RateLimiter rl;
    const std::uint64_t rate = 1000000;   // 1 MB/s, 1 MB burst
    std::size_t total = 0;
    const auto start = std::chrono::steady_clock::now();
    while (total < 3u * 1000 * 1000) {     // grant ~3 MB
        total += rl.throttle("vit", rate, 0, 65536);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // 3 MB at 1 MB/s after a 1 MB burst ~= 2 s.
    EXPECT_GT(secs, 1.0);
    EXPECT_LT(secs, 6.0);
}

TEST(RateLimiter, PerClientSharedAcrossTransfers) {
    // Two concurrent "transfers" for the SAME client must together stay near the
    // per-client rate (they share one bucket), not double it.
    RateLimiter rl;
    const std::uint64_t rate = 1000000;   // 1 MB/s
    std::atomic<std::uint64_t> total{0};
    const auto start = std::chrono::steady_clock::now();
    auto worker = [&] {
        std::uint64_t got = 0;
        while (got < 1500000) { got += rl.throttle("same-user", rate, 0, 65536); }
        total.fetch_add(got);
    };
    std::thread a(worker), b(worker);   // 2 threads, ~3 MB total for one client
    a.join(); b.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // ~3 MB shared at 1 MB/s (1 MB burst) ~= 2 s. If the bucket were per-transfer
    // it would finish in ~0.5 s (each thread its own 1 MB burst + rate).
    EXPECT_GT(secs, 1.3);
}

TEST(RateLimiter, DistinctClientsIndependent) {
    // Different clients get independent buckets: two clients each pulling at the
    // limit finish about as fast as one would (their buckets don't interfere).
    RateLimiter rl;
    const std::uint64_t rate = 2000000;   // 2 MB/s each
    EXPECT_EQ(rl.throttle("alice", rate, 0, 65536), 65536u);   // fresh burst
    EXPECT_EQ(rl.throttle("bob", rate, 0, 65536), 65536u);     // independent fresh burst
}

// --- Admin integration ------------------------------------------------------
class V2Admin : public ::testing::Test {
protected:
    void SetUp() override {
        fileshare::net::startup();
        static std::atomic<int> counter{0};
        base_ = fs::temp_directory_path() /
                ("fileshare_v2_admin_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        share_ = base_ / "share";
        out_ = base_ / "out";
        fs::create_directories(share_);
        fs::create_directories(out_);
        big_ = std::string(4u * 1024 * 1024, 'D');            // 4 MiB
        std::ofstream(share_ / "big.bin", std::ios::binary) << big_;

        Settings s;
        s.port = 5555;   // a valid port so hub.set()'s validate() passes; the
                         // listener still binds an ephemeral port via bind(0)
        s.share_root = share_.string();
        s.checksum_cache_file = "";
        s.events_enabled = false;   // not needed here; keep tests focused
        s.limits.handshake_timeout_s = 30;
        s.limits.idle_timeout_s = 30;
        ctx_ = std::make_unique<ServerContext>(s, (base_ / "config.json").string());
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

    fs::path base_, share_, out_;
    std::string big_;
    std::uint16_t port_ = 0;
    std::unique_ptr<ServerContext> ctx_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

TEST_F(V2Admin, SetChangesConfigLive) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "admin", "").ok);
    const auto r = c.admin_set("limits.per_client_bps", "12345");
    EXPECT_TRUE(r.ok) << r.message;
    // The running server's snapshot reflects it immediately.
    EXPECT_EQ(ctx_->settings()->limits.per_client_bps, 12345u);
    const std::string cfg = c.admin_get_config();
    EXPECT_NE(cfg.find("12345"), std::string::npos);
}

TEST_F(V2Admin, SetRestartKeyRejected) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "admin", "").ok);
    const auto r = c.admin_set("server.port", "9999");
    EXPECT_FALSE(r.ok);
}

TEST_F(V2Admin, SetPersistsToConfigFile) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "admin", "").ok);
    ASSERT_TRUE(c.admin_set("server.motd", "persisted!").ok);
    // The hub change callback writes config.json.
    std::this_thread::sleep_for(100ms);
    std::ifstream in(base_ / "config.json");
    ASSERT_TRUE(in.good());
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("persisted!"), std::string::npos);
}

// The flagship M11 test: lowering the limit slows a transfer, and raising it
// mid-flight speeds up an ALREADY-RUNNING download -- no restart, no reconnect.
TEST_F(V2Admin, LiveLimitChangeAffectsActiveDownload) {
    // Throttle to 256 KiB/s: 4 MiB would take ~16 s at this rate.
    Client admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", port_, "admin", "").ok);
    ASSERT_TRUE(admin.admin_set("limits.per_client_bps", "262144").ok);

    // Start the download on its own connection/thread.
    Client dl;
    ASSERT_TRUE(dl.connect("127.0.0.1", port_, "vit", "").ok);
    std::atomic<bool> done{false};
    Client::DownloadResult result;
    const auto start = std::chrono::steady_clock::now();
    std::thread t([&] {
        result = dl.download("/big.bin", (out_ / "big.bin").string());
        done = true;
    });

    // Let it run throttled for a bit, then lift the limit mid-transfer.
    std::this_thread::sleep_for(1200ms);
    EXPECT_FALSE(done.load()) << "download finished before the limit was lifted -- not throttled";
    ASSERT_TRUE(admin.admin_set("limits.per_client_bps", "0").ok);   // unlimited

    t.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.checksum_ok);
    // Far below the ~16 s it would take if the limit had stayed at 256 KiB/s.
    EXPECT_LT(secs, 10.0);
    // Content is intact despite the mid-flight change.
    std::ifstream in(out_ / "big.bin", std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(got.size(), big_.size());
}

TEST_F(V2Admin, EventConfigBroadcastToSubscriber) {
    // One admin subscribes to config events; another admin changes a key.
    Client watcher;
    ASSERT_TRUE(watcher.connect("127.0.0.1", port_, "watch", "").ok);
    std::vector<EventConfig> events;
    watcher.set_event_handler([&](const Frame& f) {
        if (f.type == Msg::EVENT_CONFIG) events.push_back(parse_event_config(f.payload.data(), f.payload.size()));
    });
    watcher.subscribe(SUB_CONFIG);
    (void)watcher.admin_stats();   // round-trip so SUBSCRIBE is processed

    Client setter;
    ASSERT_TRUE(setter.connect("127.0.0.1", port_, "admin", "").ok);
    ASSERT_TRUE(setter.admin_set("limits.global_bps", "999999").ok);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (events.empty() && std::chrono::steady_clock::now() < deadline) watcher.poll_events(50);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.front().key, "limits.global_bps");
    EXPECT_EQ(events.front().new_value, "999999");
}
