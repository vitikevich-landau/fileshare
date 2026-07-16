#pragma once

// Authorization table: the minimum role required to send each client->server
// message. The dispatcher checks this BEFORE handling, so per-message auth lives
// in one place instead of being scattered across handlers. Pure + testable.

#include "fileshare/v2/protocol.hpp"

namespace fileshare::v2 {

// Minimum role that may send `m`. Server->client-only messages (responses,
// events) return ADMIN so a client that sends one is denied/rejected rather
// than silently accepted.
[[nodiscard]] Role min_role(Msg m) noexcept;

// True if `have` satisfies `need` (ANONYMOUS < USER < ADMIN).
[[nodiscard]] inline bool role_allows(Role have, Role need) noexcept {
    return static_cast<std::uint8_t>(have) >= static_cast<std::uint8_t>(need);
}

} // namespace fileshare::v2
