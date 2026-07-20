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
#include "fileshare/v2/server.hpp"
#include "fileshare/v2/server_context.hpp"
#include "fileshare/v2/settings.hpp"

namespace fs = std::filesystem;
using namespace fileshare::v2;
using namespace std::chrono_literals;

class V2Events : public ::testing::Test {
protected:
    void SetUp() override {
        fileshare::net::startup();
        static std::atomic<int> counter{0};
        base_ = fs::temp_directory_path() /
                ("fileshare_v2_ev_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        share_ = base_ / "share";
        fs::create_directories(share_ / "sub");
        std::ofstream(share_ / "existing.txt", std::ios::binary) << "hi";

        Settings s;
        s.port = 0;
        s.share_root = share_.string();
        s.checksum_cache_file = "";
        s.events_enabled = true;
        s.events_debounce_ms = 20;         // snappy for tests
        s.limits.handshake_timeout_s = 30;
        s.limits.idle_timeout_s = 30;
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

    // Poll the client for up to `ms`, collecting EVENT_FS into `out`.
    void pump_until(Client& c, std::vector<EventFs>& out, int ms, std::size_t want = 1) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
            c.poll_events(50);
        }
    }

    fs::path base_, share_;
    std::uint16_t port_ = 0;
    std::unique_ptr<ServerContext> ctx_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

TEST_F(V2Events, CreateDeliversEventFs) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "vit", "").ok);

    std::vector<EventFs> events;
    c.set_event_handler([&](const Frame& f) {
        if (f.type == Msg::EVENT_FS) events.push_back(parse_event_fs(f.payload.data(), f.payload.size()));
    });
    c.subscribe(SUB_FS);
    (void)c.list_dir("/");            // round-trip ensures SUBSCRIBE was processed

    std::ofstream(share_ / "brandnew.bin", std::ios::binary) << std::string(1234, 'x');

    pump_until(c, events, 3000);
    ASSERT_FALSE(events.empty());
    bool found = false;
    for (const auto& e : events) if (e.path == "/brandnew.bin") found = true;
    EXPECT_TRUE(found);
}

TEST_F(V2Events, NoSubscribeNoEvents) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "vit", "").ok);
    std::vector<EventFs> events;
    c.set_event_handler([&](const Frame& f) {
        if (f.type == Msg::EVENT_FS) events.push_back(parse_event_fs(f.payload.data(), f.payload.size()));
    });
    (void)c.list_dir("/");   // no subscribe
    std::ofstream(share_ / "silent.bin", std::ios::binary) << "x";
    pump_until(c, events, 800);
    EXPECT_TRUE(events.empty());
}

TEST_F(V2Events, EventsInSubdirDelivered) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "vit", "").ok);
    std::vector<EventFs> events;
    c.set_event_handler([&](const Frame& f) {
        if (f.type == Msg::EVENT_FS) events.push_back(parse_event_fs(f.payload.data(), f.payload.size()));
    });
    c.subscribe(SUB_FS);
    (void)c.list_dir("/");
    std::ofstream(share_ / "sub" / "deep.bin", std::ios::binary) << "y";
    pump_until(c, events, 3000);
    bool found = false;
    for (const auto& e : events) if (e.path == "/sub/deep.bin") found = true;
    EXPECT_TRUE(found);
}

// Moving a populated directory tree INTO the share must be fully watched, not
// just its top level -- a file created inside a moved-in subdir emits an event.
TEST_F(V2Events, MovedInSubtreeIsWatched) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "vit", "").ok);
    std::vector<EventFs> events;
    c.set_event_handler([&](const Frame& f) {
        if (f.type == Msg::EVENT_FS) events.push_back(parse_event_fs(f.payload.data(), f.payload.size()));
    });
    c.subscribe(SUB_FS);
    (void)c.list_dir("/");

    // Build a populated tree OUTSIDE the share, then move it in.
    const fs::path staging = base_ / "staging" / "tree" / "deep";
    fs::create_directories(staging);
    std::ofstream(staging / "pre.bin", std::ios::binary) << "x";   // pre-existing
    fs::rename(base_ / "staging" / "tree", share_ / "tree");        // IN_MOVED_TO of a populated dir

    // Wait for the watcher to process IN_MOVED_TO and add the recursive watches
    // (its select loop polls on a 200ms tick), then create a NEW file inside the
    // moved-in deep subdir; its watch must now exist.
    std::this_thread::sleep_for(700ms);
    std::ofstream(share_ / "tree" / "deep" / "after.bin", std::ios::binary) << "y";

    // Poll until the specific deep-file event arrives (not just the first event,
    // which is the /tree move itself).
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    bool found = false;
    while (!found && std::chrono::steady_clock::now() < deadline) {
        c.poll_events(50);
        for (const auto& e : events) if (e.path == "/tree/deep/after.bin") found = true;
    }
    EXPECT_TRUE(found);
}

// The critical interleaving case: events buffered before a download response
// must be handled out-of-band and must not corrupt the transfer.
TEST_F(V2Events, EventDuringDownloadDoesNotCorrupt) {
    // A biggish file to download.
    const std::string content(2u * 1024 * 1024, 'Z');
    std::ofstream(share_ / "big.bin", std::ios::binary) << content;

    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "vit", "").ok);
    std::atomic<int> event_count{0};
    c.set_event_handler([&](const Frame& f) { if (f.type == Msg::EVENT_FS) ++event_count; });
    c.subscribe(SUB_FS);
    (void)c.list_dir("/");

    // Generate events, let the watcher broadcast them (they queue in the socket
    // buffer), THEN download -- so EVENT_FS frames interleave with the stream.
    for (int i = 0; i < 5; ++i) {
        std::ofstream(share_ / ("evt" + std::to_string(i) + ".bin"), std::ios::binary) << "e";
    }
    std::this_thread::sleep_for(300ms);

    const fs::path dst = base_ / "big.out";
    const auto r = c.download("/big.bin", dst.string());
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.checksum_ok);
    std::ifstream in(dst, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(got.size(), content.size());
    EXPECT_EQ(got, content);
    EXPECT_GT(event_count.load(), 0);   // events really did arrive interleaved
}

// A second client's activity does not disturb an idle subscriber, and idle
// event polling coexists with request/response.
TEST_F(V2Events, PollThenRequestStillWorks) {
    Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port_, "vit", "").ok);
    c.subscribe(SUB_FS);
    (void)c.list_dir("/");
    // Poll (no events pending) then issue a normal request -- must still work.
    c.poll_events(50);
    const auto entries = c.list_dir("/");
    EXPECT_GE(entries.size(), 2u);
}
