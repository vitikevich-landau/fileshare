#include "fileshare/v2/rate_limiter.hpp"

#include <algorithm>
#include <thread>

namespace fileshare::v2 {

namespace {
using clock = std::chrono::steady_clock;

// Add tokens for the elapsed time, capped at one second's worth (burst bound).
void refill(TokenBucket& b, double rate, clock::time_point now) {
    if (!b.initialized) {
        b.initialized = true;
        b.last = now;
        b.tokens = rate;   // allow an initial ~1s burst
        return;
    }
    const double dt = std::chrono::duration<double>(now - b.last).count();
    b.last = now;
    b.tokens = std::min(rate, b.tokens + rate * dt);
}
} // namespace

std::size_t RateLimiter::throttle(TokenBucket& local, std::uint64_t per_client_bps,
                                  std::uint64_t global_bps, std::size_t want) {
    if (want == 0) return 0;
    if (per_client_bps == 0 && global_bps == 0) return want;   // unlimited

    for (;;) {
        const auto now = clock::now();

        double local_avail = static_cast<double>(want);
        if (per_client_bps > 0) {
            refill(local, static_cast<double>(per_client_bps), now);
            local_avail = local.tokens;
        }

        std::size_t granted = 0;
        {
            std::lock_guard<std::mutex> lk(gmu_);
            double global_avail = static_cast<double>(want);
            if (global_bps > 0) {
                refill(global_, static_cast<double>(global_bps), now);
                global_avail = global_.tokens;
            }
            const double g = std::min({static_cast<double>(want), local_avail, global_avail});
            if (g >= 1.0) {
                granted = static_cast<std::size_t>(g);
                if (global_bps > 0) global_.tokens -= static_cast<double>(granted);
            }
        }

        if (granted >= 1) {
            if (per_client_bps > 0) local.tokens -= static_cast<double>(granted);
            return granted;
        }
        // Not enough budget yet; wait for the buckets to refill.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace fileshare::v2
