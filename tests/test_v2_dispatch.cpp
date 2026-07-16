#include <gtest/gtest.h>

#include "fileshare/v2/dispatch.hpp"
#include "fileshare/v2/settings.hpp"

using namespace fileshare::v2;

// --- Role gating ------------------------------------------------------------
TEST(V2Dispatch, AnonymousMessages) {
    EXPECT_EQ(min_role(Msg::HELLO), Role::ANONYMOUS);
    EXPECT_EQ(min_role(Msg::AUTH_REQUEST), Role::ANONYMOUS);
    EXPECT_EQ(min_role(Msg::PING), Role::ANONYMOUS);
}

TEST(V2Dispatch, UserMessages) {
    EXPECT_EQ(min_role(Msg::LIST_DIR_REQUEST), Role::USER);
    EXPECT_EQ(min_role(Msg::DOWNLOAD_REQUEST), Role::USER);
    EXPECT_EQ(min_role(Msg::SUBSCRIBE), Role::USER);
}

TEST(V2Dispatch, AdminMessages) {
    EXPECT_EQ(min_role(Msg::ADMIN_SET), Role::ADMIN);
    EXPECT_EQ(min_role(Msg::ADMIN_KICK), Role::ADMIN);
    EXPECT_EQ(min_role(Msg::ADMIN_SHUTDOWN), Role::ADMIN);
}

TEST(V2Dispatch, ServerOnlyMessagesRequireAdmin) {
    // A client must not send responses/events; treat as admin-gated so a plain
    // user cannot smuggle them in.
    EXPECT_EQ(min_role(Msg::LIST_DIR_RESPONSE), Role::ADMIN);
    EXPECT_EQ(min_role(Msg::EVENT_FS), Role::ADMIN);
}

TEST(V2Dispatch, RoleOrdering) {
    EXPECT_TRUE(role_allows(Role::ADMIN, Role::USER));
    EXPECT_TRUE(role_allows(Role::ADMIN, Role::ADMIN));
    EXPECT_TRUE(role_allows(Role::USER, Role::USER));
    EXPECT_FALSE(role_allows(Role::USER, Role::ADMIN));
    EXPECT_FALSE(role_allows(Role::ANONYMOUS, Role::USER));
}

// --- Settings ---------------------------------------------------------------
TEST(V2Settings, DefaultsAreValid) {
    Settings s;
    EXPECT_TRUE(s.validate().empty());
    EXPECT_EQ(s.port, 5555);
}

TEST(V2Settings, ValidationCatchesBadRanges) {
    Settings s;
    s.port = 0;
    EXPECT_FALSE(s.validate().empty());

    Settings s2;
    s2.limits.global_bps = 1000;
    s2.limits.per_client_bps = 5000;   // per-client > global
    EXPECT_FALSE(s2.validate().empty());

    Settings s3;
    s3.limits.idle_timeout_s = 0;
    EXPECT_FALSE(s3.validate().empty());
}

TEST(V2Settings, JsonRoundTrip) {
    Settings s;
    s.port = 6000;
    s.share_root = "/srv/data";
    s.motd = "hi";
    s.limits.per_client_bps = 10485760;
    s.limits.max_connections = 42;

    const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                            "/fileshare_settings_test.json";
    s.save(tmp);
    const Settings loaded = Settings::load(tmp);
    EXPECT_EQ(loaded.port, 6000);
    EXPECT_EQ(loaded.share_root, "/srv/data");
    EXPECT_EQ(loaded.motd, "hi");
    EXPECT_EQ(loaded.limits.per_client_bps, 10485760u);
    EXPECT_EQ(loaded.limits.max_connections, 42u);
    std::remove(tmp.c_str());
}

TEST(V2Settings, MissingFileYieldsDefaults) {
    const Settings s = Settings::load("/nonexistent/path/to/config.json");
    EXPECT_EQ(s.port, 5555);   // defaults, not an error
}
