#include "fileshare/v2/dispatch.hpp"

namespace fileshare::v2 {

Role min_role(Msg m) noexcept {
    switch (m) {
        // Handshake / service: no auth required.
        case Msg::HELLO:
        case Msg::AUTH_REQUEST:
        case Msg::PING:
        case Msg::PONG:
            return Role::ANONYMOUS;

        // Filesystem + transfer + events: an authenticated user.
        case Msg::LIST_DIR_REQUEST:
        case Msg::STAT_REQUEST:
        case Msg::CHECKSUM_REQUEST:
        case Msg::DOWNLOAD_REQUEST:
        case Msg::DOWNLOAD_CANCEL:
        case Msg::SUBSCRIBE:
            return Role::USER;

        // Admin channel.
        case Msg::ADMIN_GET_CONFIG:
        case Msg::ADMIN_SET:
        case Msg::ADMIN_LIST_CLIENTS:
        case Msg::ADMIN_KICK:
        case Msg::ADMIN_STATS:
        case Msg::ADMIN_SHUTDOWN:
            return Role::ADMIN;

        // Everything else is server->client only; a client must not send it.
        default:
            return Role::ADMIN;
    }
}

} // namespace fileshare::v2
