#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "fileshare/v2/tui/model.hpp"

namespace fs = std::filesystem;
using namespace fileshare::v2::tui;

namespace {

ResListing listing(int panel, const std::string& path, std::vector<PanelEntry> e) {
    return ResListing{panel, path, std::move(e)};
}

PanelEntry file(const std::string& n, std::uint64_t sz, std::uint64_t mt = 0) {
    return PanelEntry{n, false, sz, mt, false, false};
}
PanelEntry dir(const std::string& n) { return PanelEntry{n, true, 0, 0, false, false}; }

} // namespace

// --- Formatting -------------------------------------------------------------
TEST(TuiFormat, HumanSize) {
    EXPECT_EQ(human_size(0), "0B");
    EXPECT_EQ(human_size(512), "512B");
    EXPECT_EQ(human_size(1024), "1.0K");
    EXPECT_EQ(human_size(1536), "1.5K");
    EXPECT_EQ(human_size(4ull * 1024 * 1024 * 1024), "4.0G");
}

TEST(TuiFormat, HumanEta) {
    EXPECT_EQ(human_eta(65), "01:05");
    EXPECT_EQ(human_eta(3661), "1:01:01");
}

// --- Listing + sorting ------------------------------------------------------
TEST(TuiModel, ApplyListingDirsFirstWithDotDot) {
    AppState app;
    app.toggle_active();                 // make panel 1 (remote) active
    app.apply(listing(1, "/incoming", {file("b.txt", 10), dir("zzz"), file("a.txt", 20), dir("aaa")}));
    const Panel& p = app.active();
    ASSERT_EQ(p.entries.size(), 5u);     // .. + 4
    EXPECT_EQ(p.entries[0].name, "..");
    EXPECT_EQ(p.entries[1].name, "aaa"); // dirs first, name-sorted
    EXPECT_EQ(p.entries[2].name, "zzz");
    EXPECT_EQ(p.entries[3].name, "a.txt");
    EXPECT_EQ(p.entries[4].name, "b.txt");
}

TEST(TuiModel, RootListingHasNoDotDot) {
    AppState app;
    app.toggle_active();
    app.apply(listing(1, "/", {file("x", 1)}));
    EXPECT_EQ(app.active().entries.front().name, "x");
}

TEST(TuiModel, NewFlagFromLastSeen) {
    AppState app;
    app.toggle_active();
    app.panel(1).last_seen = 1000;
    app.apply(listing(1, "/", {file("old", 1, 500), file("fresh", 1, 2000)}));
    const Panel& p = app.active();
    // find fresh
    bool fresh_new = false, old_new = false;
    for (const auto& e : p.entries) {
        if (e.name == "fresh") fresh_new = e.is_new;
        if (e.name == "old") old_new = e.is_new;
    }
    EXPECT_TRUE(fresh_new);
    EXPECT_FALSE(old_new);
    EXPECT_EQ(p.new_count(), 1u);
}

// --- Navigation -------------------------------------------------------------
TEST(TuiModel, CursorMovementClamps) {
    AppState app;
    app.toggle_active();
    app.apply(listing(1, "/", {file("a", 1), file("b", 1), file("c", 1)}));
    app.cursor_end();
    EXPECT_EQ(app.active().cursor, 2u);
    app.move_cursor(5);
    EXPECT_EQ(app.active().cursor, 2u);   // clamped
    app.cursor_home();
    EXPECT_EQ(app.active().cursor, 0u);
    app.move_cursor(-5);
    EXPECT_EQ(app.active().cursor, 0u);
}

TEST(TuiModel, TabTogglesActive) {
    AppState app;
    EXPECT_EQ(app.active_index(), 0);
    app.toggle_active();
    EXPECT_EQ(app.active_index(), 1);
}

// --- Marking ----------------------------------------------------------------
TEST(TuiModel, MarkAdvancesAndCounts) {
    AppState app;
    app.toggle_active();
    app.apply(listing(1, "/", {file("a", 1), file("b", 1), file("c", 1)}));
    app.cursor_home();
    app.toggle_mark_current();            // marks "a", advances to "b"
    app.toggle_mark_current();            // marks "b", advances to "c"
    EXPECT_EQ(app.active().marked_count(), 2u);
    EXPECT_EQ(app.active().cursor, 2u);
}

TEST(TuiModel, DotDotNotMarkable) {
    AppState app;
    app.toggle_active();
    app.apply(listing(1, "/sub", {file("a", 1)}));   // has ".."
    app.cursor_home();                                // on ".."
    app.toggle_mark_current();
    EXPECT_EQ(app.active().marked_count(), 0u);
}

// --- Enter navigation -------------------------------------------------------
TEST(TuiModel, EnterRemoteDirEmitsListDir) {
    AppState app;
    app.toggle_active();
    app.panel(1).profile = "vps";
    app.apply(listing(1, "/", {dir("video")}));
    app.cursor_home();                                // on "video"
    auto cmd = app.enter();
    ASSERT_TRUE(cmd.has_value());
    ASSERT_TRUE(std::holds_alternative<CmdListDir>(*cmd));
    EXPECT_EQ(std::get<CmdListDir>(*cmd).path, "/video");
    EXPECT_TRUE(app.active().loading);
}

TEST(TuiModel, EnterRemoteDotDotGoesUp) {
    AppState app;
    app.toggle_active();
    app.apply(listing(1, "/a/b", {file("x", 1)}));
    app.cursor_home();                                // on ".."
    auto cmd = app.enter();
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(std::get<CmdListDir>(*cmd).path, "/a");
}

TEST(TuiModel, EnterFileIsNoop) {
    AppState app;
    app.toggle_active();
    app.apply(listing(1, "/", {file("x", 1)}));
    app.cursor_home();
    EXPECT_FALSE(app.enter().has_value());
}

// --- Local listing ----------------------------------------------------------
TEST(TuiModel, LoadLocalDirectory) {
    const fs::path base = fs::temp_directory_path() /
                          ("fileshare_tui_" + std::to_string(::getpid()));
    fs::create_directories(base / "sub");
    std::ofstream(base / "f.txt") << "hi";
    AppState app;
    ASSERT_TRUE(app.load_local(0, base.string()));
    const Panel& p = app.panel(0);
    // .. + sub + f.txt
    ASSERT_EQ(p.entries.size(), 3u);
    EXPECT_EQ(p.entries[0].name, "..");
    EXPECT_EQ(p.entries[1].name, "sub");   // dir first
    EXPECT_EQ(p.entries[2].name, "f.txt");
    std::error_code ec; fs::remove_all(base, ec);
}

// --- Download command -------------------------------------------------------
TEST(TuiModel, MakeDownloadFromRemoteToLocal) {
    AppState app;                          // panel0 local (active), panel1 remote
    app.load_local(0, fs::temp_directory_path().string());
    app.panel(1).source = Source::REMOTE;
    app.apply(listing(1, "/video", {file("big.iso", 1000)}));
    app.toggle_active();                   // remote active
    app.active().cursor = 0;               // ".." is not present? path=/video has ".."
    // move to the file
    for (auto& e : app.active().entries) { (void)e; }
    // find big.iso index
    auto& p = app.active();
    for (std::size_t i = 0; i < p.entries.size(); ++i) if (p.entries[i].name == "big.iso") p.cursor = i;
    auto cmd = app.make_download();
    ASSERT_TRUE(cmd.has_value());
    const auto& d = std::get<CmdDownload>(*cmd);
    EXPECT_EQ(d.remote, "/video/big.iso");
    EXPECT_EQ(d.display, "big.iso");
    EXPECT_NE(d.local.find("big.iso"), std::string::npos);
}

TEST(TuiModel, MakeDownloadRejectsLocalActive) {
    AppState app;                          // panel0 local active
    auto cmd = app.make_download();
    EXPECT_FALSE(cmd.has_value());         // active panel is local -> refused, logged
    EXPECT_FALSE(app.op_log().empty());
}

// --- Result application -----------------------------------------------------
TEST(TuiModel, ResultLogAndProgress) {
    AppState app;
    app.apply(ResInfo{"hello", true});
    ASSERT_EQ(app.op_log().size(), 1u);
    EXPECT_EQ(app.op_log().back().level, LogLevel::GOOD);

    app.apply(ResProgress{"big.iso", 500, 1000, 1000000});
    ASSERT_TRUE(app.progress().has_value());
    EXPECT_EQ(app.progress()->done, 500u);

    app.apply(ResDownloadDone{true, true, "big.iso", ""});
    EXPECT_FALSE(app.progress().has_value());
    EXPECT_EQ(app.op_log().back().level, LogLevel::GOOD);
}

TEST(TuiModel, DisconnectSetsLinkDown) {
    AppState app;
    app.apply(ResDisconnected{"peer reset"});
    EXPECT_EQ(app.link(), Link::DOWN);
    EXPECT_EQ(app.op_log().back().level, LogLevel::ERROR);
}
