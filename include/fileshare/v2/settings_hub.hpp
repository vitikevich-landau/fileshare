#pragma once

// Hot configuration via immutable snapshots (see docs/v2/03-server-daemon.md §3).
// Readers on the hot path take current() -- a single atomic load of a
// shared_ptr<const Settings> -- so a change applies on the very next read with
// no lock. Writers (admin protocol / SIGHUP reload) build a validated copy and
// atomically swap it in; the old snapshot lives on until its last reader drops it.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "fileshare/v2/settings.hpp"

namespace fileshare::v2 {

class SettingsHub {
public:
    // Called after a successful change: (key, new_value). Used to persist and to
    // broadcast EVENT_CONFIG.
    using ChangeCb = std::function<void(const std::string& key, const std::string& value)>;

    explicit SettingsHub(Settings initial)
        : snapshot_(std::make_shared<const Settings>(std::move(initial))) {}

    // Hot-path read: one atomic load, never blocks.
    [[nodiscard]] std::shared_ptr<const Settings> current() const { return snapshot_.load(); }

    // Replace the whole snapshot (validated). Returns "" on success or an error.
    // Does NOT invoke the change callback (used by SIGHUP whole-file reload).
    std::string apply(const Settings& next);

    // Change one hot key (dotted, e.g. "limits.per_client_bps"). Validates,
    // swaps, and fires the change callback. Restart-only keys are refused.
    // `old_value_out` (optional) receives the previous value for audit logs.
    std::string set(const std::string& key, const std::string& value,
                    std::string* old_value_out = nullptr);

    void set_change_cb(ChangeCb cb) { cb_ = std::move(cb); }

    // Whitelist: only these keys may be changed over the wire.
    [[nodiscard]] static bool is_hot_key(const std::string& key);
    // Current value of a key as a string (for audit / display).
    [[nodiscard]] static std::string value_of(const Settings& s, const std::string& key);

private:
    std::atomic<std::shared_ptr<const Settings>> snapshot_;
    std::mutex write_mu_;   // serializes writers so set() is read-modify-write safe
    ChangeCb   cb_;
};

} // namespace fileshare::v2
