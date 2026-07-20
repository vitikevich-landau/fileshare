#include "fileshare/v2/rate_limiter.hpp"

#include <algorithm>
#include <thread>

namespace fileshare::v2 {

namespace {
using clock = std::chrono::steady_clock;

// Add tokens for the elapsed time, capped at one second's worth (burst bound).
// Caller holds the bucket's mutex.
void refill(RateLimiter* /*self*/, double rate, clock::time_point now,
            double& tokens, clock::time_point& last, bool& initialized) {
    if (!initialized) {
        initialized = true;
        last = now;
        tokens = rate;   // allow an initial ~1s burst
        return;
    }
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    tokens = std::min(rate, tokens + rate * dt);
}
} // namespace

std::shared_ptr<RateLimiter::Bucket> RateLimiter::client_bucket(const std::string& key) {
    std::lock_guard<std::mutex> lk(map_mu_);
    auto& slot = per_client_[key];
    if (!slot) slot = std::make_shared<Bucket>();
    return slot;
}

std::size_t RateLimiter::throttle(const std::string& client_key, std::uint64_t per_client_bps,
                                  std::uint64_t global_bps, std::size_t want) {
    if (want == 0) return 0;
    if (per_client_bps == 0 && global_bps == 0) return want;   // unlimited

    // One shared bucket per client (across all their concurrent transfers).
    std::shared_ptr<Bucket> pc = (per_client_bps > 0) ? client_bucket(client_key) : nullptr;

    for (;;) {
        const auto now = clock::now();
        std::size_t granted = 0;

        // Consistent lock order (per-client bucket first, then global) avoids
        // deadlock. When there is no per-client limit, only the global bucket
        // is locked (once, by the inner guard).
        std::unique_lock<std::mutex> pcl;
        if (pc) pcl = std::unique_lock<std::mutex>(pc->mu);
        {
            std::lock_guard<std::mutex> gl(global_.mu);
            double pc_avail = static_cast<double>(want);
            if (pc) {
                refill(this, static_cast<double>(per_client_bps), now, pc->tokens, pc->last, pc->initialized);
                pc_avail = pc->tokens;
            }
            double global_avail = static_cast<double>(want);
            if (global_bps > 0) {
                refill(this, static_cast<double>(global_bps), now,
                       global_.tokens, global_.last, global_.initialized);
                global_avail = global_.tokens;
            }
            const double g = std::min({static_cast<double>(want), pc_avail, global_avail});
            if (g >= 1.0) {
                granted = static_cast<std::size_t>(g);
                if (pc) pc->tokens -= static_cast<double>(granted);
                if (global_bps > 0) global_.tokens -= static_cast<double>(granted);
            }
        }
        if (pc) pcl.unlock();

        if (granted >= 1) return granted;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));   // wait for refill
    }
}

} // namespace fileshare::v2
