#include <gtest/gtest.h>

#include <string>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "fileshare/v2/tui/model.hpp"
#include "fileshare/v2/tui/view.hpp"

using namespace fileshare::v2::tui;
using namespace ftxui;
using fileshare::v2::AdminStats;
using fileshare::v2::AdminClientInfo;
using fileshare::v2::Role;

namespace {

// Render an element to a plain string (no terminal needed).
std::string to_string(Element e, int w = 100, int h = 30) {
    auto screen = Screen::Create(Dimension::Fixed(w), Dimension::Fixed(h));
    Render(screen, e);
    return screen.ToString();
}

AppState make_populated() {
    AppState app;
    // Local panel 0.
    app.panel(0).source = Source::LOCAL;
    app.panel(0).path = "/home/vit";
    app.panel(0).entries = {
        PanelEntry{"..", true, 0, 0, false, false},
        PanelEntry{"docs", true, 0, 1000, false, false},
        PanelEntry{"report.pdf", false, 1258291, 1000, false, false},
    };
    // Remote panel 1 with a "new" file.
    app.panel(1).source = Source::REMOTE;
    app.panel(1).profile = "vps";
    app.panel(1).path = "/incoming";
    app.panel(1).entries = {
        PanelEntry{"..", true, 0, 0, false, false},
        PanelEntry{"new-build.tar.gz", false, 356515840, 2000, true, false},
        PanelEntry{"readme.txt", false, 2048, 500, false, false},
    };
    return app;
}

} // namespace

TEST(TuiView, RendersBothPanelTitles) {
    AppState app = make_populated();
    const std::string s = to_string(render_commander(app, /*admin=*/true, "vit@vps:/incoming$ "));
    EXPECT_NE(s.find("/home/vit"), std::string::npos);
    EXPECT_NE(s.find("fs://vps/incoming"), std::string::npos);
}

TEST(TuiView, RendersEntriesAndPrompt) {
    AppState app = make_populated();
    const std::string s = to_string(render_commander(app, true, "vit@vps:/incoming$ "));
    EXPECT_NE(s.find("report.pdf"), std::string::npos);
    EXPECT_NE(s.find("new-build.tar.gz"), std::string::npos);
    EXPECT_NE(s.find("vit@vps:/incoming$"), std::string::npos);
}

TEST(TuiView, ShowsFunctionBarAndAdminKey) {
    AppState app = make_populated();
    const std::string admin_s = to_string(render_commander(app, true, "$ "));
    EXPECT_NE(admin_s.find("Quit"), std::string::npos);
    EXPECT_NE(admin_s.find("Admin"), std::string::npos);

    const std::string user_s = to_string(render_commander(app, false, "$ "));
    EXPECT_EQ(user_s.find("Admin"), std::string::npos);   // no admin key for a user
}

TEST(TuiView, ShowsProgressGauge) {
    AppState app = make_populated();
    app.set_progress(ResProgress{"big.iso", 500u * 1024 * 1024, 1024u * 1024 * 1024, 12000000});
    const std::string s = to_string(render_commander(app, false, "$ "));
    EXPECT_NE(s.find("big.iso"), std::string::npos);
    EXPECT_NE(s.find("eta"), std::string::npos);
}

TEST(TuiView, ShowsErrorLogLine) {
    AppState app = make_populated();
    app.log(LogLevel::ERROR, "connection lost: peer reset");
    const std::string s = to_string(render_commander(app, false, "$ "));
    EXPECT_NE(s.find("connection lost"), std::string::npos);
}

TEST(TuiView, EmptyStateDoesNotCrash) {
    AppState app;   // both panels empty
    const std::string s = to_string(render_commander(app, false, "$ "));
    EXPECT_FALSE(s.empty());
}

// --- Admin panel ------------------------------------------------------------
TEST(TuiAdminView, OverviewShowsStats) {
    AppState app;
    app.open_admin();
    app.admin_set_tab(AdminTab::OVERVIEW);
    AdminStats st;
    st.version = "2.0.0";
    st.active_connections = 3;
    st.per_client_bps = 10485760;
    st.global_bps = 0;
    app.apply(ResAdminStats{st});
    const std::string s = to_string(render_admin(app));
    EXPECT_NE(s.find("ADMIN"), std::string::npos);
    EXPECT_NE(s.find("Overview"), std::string::npos);
    EXPECT_NE(s.find("2.0.0"), std::string::npos);
    EXPECT_NE(s.find("unlimited"), std::string::npos);   // global limit 0
}

TEST(TuiAdminView, ClientsTabListsAndKickHint) {
    AppState app;
    app.open_admin();
    app.admin_set_tab(AdminTab::CLIENTS);
    AdminClientInfo c{7, "vit", "1.2.3.4", Role::ADMIN, "/big.iso", 100, 0};
    app.apply(ResAdminClients{{c}});
    const std::string s = to_string(render_admin(app));
    EXPECT_NE(s.find("vit"), std::string::npos);
    EXPECT_NE(s.find("1.2.3.4"), std::string::npos);
    EXPECT_NE(s.find("Kick"), std::string::npos);
}

TEST(TuiAdminView, SettingsTabParsesConfig) {
    AppState app;
    app.open_admin();
    app.admin_set_tab(AdminTab::SETTINGS);
    const std::string cfg = R"({"limits":{"per_client_bps":12345},"server":{"port":5555,"motd":"hi"},
                                 "log":{"level":"info"},"events":{"debounce_ms":500}})";
    app.apply(ResAdminConfig{cfg});
    const std::string s = to_string(render_admin(app));
    EXPECT_NE(s.find("limits.per_client_bps"), std::string::npos);
    EXPECT_NE(s.find("12345"), std::string::npos);
    EXPECT_NE(s.find("[hot]"), std::string::npos);
    EXPECT_NE(s.find("[restart]"), std::string::npos);   // server.port is restart-only
}

TEST(TuiAdminModel, SettingsSelectionRespectsHotFlag) {
    AppState app;
    app.open_admin();
    app.admin_set_tab(AdminTab::SETTINGS);
    app.apply(ResAdminConfig{R"({"server":{"port":5555}})"});
    // find the server.port row and select it -> should be flagged restart (not hot)
    for (std::size_t i = 0; i < app.admin().settings.size(); ++i) {
        if (std::get<0>(app.admin().settings[i]) == "server.port") { app.admin_move(static_cast<int>(i)); break; }
    }
    auto sel = app.admin_selected_setting();
    ASSERT_TRUE(sel.has_value());
    EXPECT_EQ(sel->first, "server.port");
    EXPECT_FALSE(sel->second);   // not hot -> the UI refuses to edit it
}
