#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace fileshare {

// Parse a TCP port from a CLI string without throwing. Accepts only decimal
// digits with a value in [1, 65535]; returns std::nullopt on empty input,
// non-numeric text, trailing garbage, a negative sign, overflow, or 0. Used by
// both console entrypoints so bad --port input yields a clean usage error
// instead of an uncaught std::stoi exception.
[[nodiscard]] std::optional<std::uint16_t> parse_port(std::string_view text);

} // namespace fileshare
