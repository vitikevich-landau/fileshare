#pragma once

// Recursive filesystem watcher over the share root (Linux inotify). Emits
// created/modified/removed events (debounced) so the server can push EVENT_FS to
// subscribers and invalidate the checksum cache. On non-Linux it is an inert
// stub (active() == false) and events are simply disabled.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "fileshare/v2/protocol.hpp"

namespace fileshare::v2 {

class FsWatcher {
public:
    // vpath is the share-relative path ("/incoming/x.bin"). Called from the
    // watcher thread.
    using Callback = std::function<void(FsOp op, EntryKind kind, const std::string& vpath,
                                        std::uint64_t size, std::uint64_t mtime)>;

    FsWatcher(std::filesystem::path root, std::uint32_t debounce_ms, Callback cb);
    ~FsWatcher();
    FsWatcher(const FsWatcher&) = delete;
    FsWatcher& operator=(const FsWatcher&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    void run();

    std::filesystem::path root_;
    std::uint32_t         debounce_ms_;
    Callback              cb_;
    std::thread           thread_;
    bool                  active_ = false;
    std::atomic<bool>     stop_{false};
    int                   inotify_fd_ = -1;
};

} // namespace fileshare::v2
