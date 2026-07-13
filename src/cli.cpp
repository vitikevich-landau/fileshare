#include "fileshare/cli.hpp"

#include <charconv>

namespace fileshare {

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt; // non-numeric, trailing garbage, negative, or overflow
    }
    if (value < 1u || value > 65535u) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace fileshare
