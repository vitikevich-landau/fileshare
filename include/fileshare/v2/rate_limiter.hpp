#pragma once

// Token-bucket bandwidth limiting for the thread-per-connection server. A
// download thread asks throttle() how many bytes it may send now; the limit is
// read fresh on every call, so lowering limits.per_client_bps / global_bps from
// the admin panel slows an in-flight transfer within one chunk -- no restart,
// no reconnect. Rate 0 means unlimited.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace fileshare::v2 {

// One transfer's local bucket (single-threaded; no lock needed).
struct TokenBucket {
    double tokens = 0.0;
    std::chrono::steady_clock::time_point last{};
    bool initialized = false;
};

class RateLimiter {
public:
    // Grant up to `want` bytes, throttled to `per_client_bps` (via `local`) and
    // the shared global rate. Blocks in short sleeps until >= 1 byte is
    // grantable, then returns the granted amount (<= want). Unlimited when both
    // rates are 0.
    std::size_t throttle(TokenBucket& local, std::uint64_t per_client_bps,
                         std::uint64_t global_bps, std::size_t want);

private:
    std::mutex  gmu_;
    TokenBucket global_;
};

} // namespace fileshare::v2
