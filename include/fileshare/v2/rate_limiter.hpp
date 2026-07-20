#pragma once

// Token-bucket bandwidth limiting for the thread-per-connection server. A
// download thread asks throttle() how many bytes it may send now; the limit is
// read fresh on every call, so lowering limits.per_client_bps / global_bps from
// the admin panel slows an in-flight transfer within one chunk -- no restart,
// no reconnect. Rate 0 means unlimited.
//
// per_client_bps is enforced PER CLIENT (keyed by login), not per transfer: all
// of one client's concurrent downloads share a single bucket, so N parallel
// transfers cannot together exceed the per-client limit. A separate global
// bucket bounds the whole server.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fileshare::v2 {

class RateLimiter {
public:
    // Grant up to `want` bytes for `client_key`'s traffic, throttled to
    // `per_client_bps` (shared across that client's transfers) and the server
    // `global_bps`. Blocks in short sleeps until >= 1 byte is grantable, then
    // returns the granted amount (<= want). Unlimited when both rates are 0.
    std::size_t throttle(const std::string& client_key, std::uint64_t per_client_bps,
                         std::uint64_t global_bps, std::size_t want);

private:
    struct Bucket {
        std::mutex  mu;
        double      tokens = 0.0;
        std::chrono::steady_clock::time_point last{};
        bool        initialized = false;
    };

    std::shared_ptr<Bucket> client_bucket(const std::string& key);

    std::mutex map_mu_;
    std::unordered_map<std::string, std::shared_ptr<Bucket>> per_client_;
    Bucket global_;
};

} // namespace fileshare::v2
