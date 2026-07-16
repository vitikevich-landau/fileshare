#include <gtest/gtest.h>

#include <string>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "fileshare/v2/tui/model.hpp"
#include "fileshare/v2/tui/view.hpp"

using namespace fileshare::v2::tui;
using namespace ftxui;

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
