#include <gtest/gtest.h>

#include "fileshare/cli.hpp"

using namespace fileshare;

TEST(Cli, ParsePortValid) {
    EXPECT_EQ(parse_port("1"), 1);
    EXPECT_EQ(parse_port("5555"), 5555);
    EXPECT_EQ(parse_port("65535"), 65535);
}

TEST(Cli, ParsePortRejectsOutOfRange) {
    EXPECT_FALSE(parse_port("0").has_value());       // 0 reserved (OS-assigned)
    EXPECT_FALSE(parse_port("65536").has_value());   // just past u16
    EXPECT_FALSE(parse_port("70000").has_value());   // would silently truncate
    EXPECT_FALSE(parse_port("99999999999").has_value()); // would overflow std::stoi (int)
}

TEST(Cli, ParsePortRejectsNonNumeric) {
    EXPECT_FALSE(parse_port("").has_value());
    EXPECT_FALSE(parse_port("abc").has_value());
    EXPECT_FALSE(parse_port("-1").has_value());
    EXPECT_FALSE(parse_port("80x").has_value());     // trailing garbage
    EXPECT_FALSE(parse_port(" 80").has_value());     // leading space
    EXPECT_FALSE(parse_port("0x50").has_value());
}
