#include "fileshare/v2/fs_watcher.hpp"

#include <chrono>
#include <system_error>

#include "fileshare/v2/log.hpp"

#if defined(__linux__)
#  include <cerrno>
#  include <cstring>
#  include <map>
#  include <sys/inotify.h>
#  include <sys/select.h>
#  include <unistd.h>
#  include <unordered_map>
#endif

namespace fs = std::filesystem;

namespace fileshare::v2 {

FsWatcher::FsWatcher(fs::path root, std::uint32_t debounce_ms, Callback cb)
    : root_(std::move(root)), debounce_ms_(debounce_ms), cb_(std::move(cb)) {}

FsWatcher::~FsWatcher() { stop(); }

#if defined(__linux__)

namespace {
constexpr std::uint32_t kMask =
    IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM;

std::uint64_t stat_size(const fs::path& p, bool is_dir) {
    if (is_dir) return 0;
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : n;
}
std::uint64_t stat_mtime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    return secs < 0 ? 0 : static_cast<std::uint64_t>(secs);
}
} // namespace

void FsWatcher::start() {
    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) {
        log_warn("inotify unavailable; live events disabled");
        return;
    }
    active_ = true;
    thread_ = std::thread([this] { run(); });
}

void FsWatcher::stop() {
    if (!active_ && inotify_fd_ < 0) return;
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (inotify_fd_ >= 0) { ::close(inotify_fd_); inotify_fd_ = -1; }
    active_ = false;
}

void FsWatcher::run() {
    std::map<int, fs::path> wd_to_dir;   // watch descriptor -> directory path

    std::function<void(const fs::path&)> add_watch_recursive = [&](const fs::path& dir) {
        std::error_code ec;
        // Watch the directory itself...
        const int wd = ::inotify_add_watch(inotify_fd_, dir.c_str(), kMask);
        if (wd >= 0) wd_to_dir[wd] = dir;
        // ...and every existing subdirectory.
        fs::recursive_directory_iterator it(
            dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) {
                const int w = ::inotify_add_watch(inotify_fd_, it->path().c_str(), kMask);
                if (w >= 0) wd_to_dir[w] = it->path();
            }
        }
    };
    add_watch_recursive(root_);

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_emit;
    auto vpath_of = [&](const fs::path& full) {
        std::error_code ec;
        fs::path rel = fs::relative(full, root_, ec);
        if (ec) return std::string("/");
        std::string s = "/" + rel.generic_string();
        return s;
    };

    std::vector<char> buf(64 * 1024);
    while (!stop_.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(inotify_fd_, &rd);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        const int n = ::select(inotify_fd_ + 1, &rd, nullptr, nullptr, &tv);
        if (n <= 0) continue;

        const ssize_t len = ::read(inotify_fd_, buf.data(), buf.size());
        if (len <= 0) continue;

        for (char* p = buf.data(); p < buf.data() + len;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;

            // A watch was removed (dir deleted/moved/unmounted): drop its entry
            // so wd_to_dir doesn't leak and a reused wd can't map to a stale dir.
            if (ev->mask & IN_IGNORED) {
                wd_to_dir.erase(ev->wd);
                continue;
            }
            if (ev->len == 0) continue;

            auto dir_it = wd_to_dir.find(ev->wd);
            if (dir_it == wd_to_dir.end()) continue;
            const fs::path full = dir_it->second / std::string(ev->name);
            const bool is_dir = (ev->mask & IN_ISDIR) != 0;
            const std::string vpath = vpath_of(full);

            // A directory appeared (created or moved in): watch it AND all of its
            // pre-existing subdirectories, so moving a populated tree into the
            // share is fully covered, not just its top level.
            if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                add_watch_recursive(full);
            }

            FsOp op;
            if (ev->mask & (IN_DELETE | IN_MOVED_FROM))      op = FsOp::REMOVED;
            else if (ev->mask & (IN_CREATE | IN_MOVED_TO))   op = FsOp::CREATED;
            else if (ev->mask & IN_CLOSE_WRITE)              op = FsOp::MODIFIED;
            else                                             continue;

            // For a plain file, ignore the bare IN_CREATE and act on the later
            // IN_CLOSE_WRITE, so we don't announce a half-written file.
            if (!is_dir && (ev->mask & IN_CREATE) && !(ev->mask & IN_CLOSE_WRITE)) {
                continue;
            }

            // Debounce identical paths within the window.
            const auto now = std::chrono::steady_clock::now();
            auto le = last_emit.find(vpath);
            if (le != last_emit.end() &&
                now - le->second < std::chrono::milliseconds(debounce_ms_)) {
                continue;
            }
            last_emit[vpath] = now;

            const EntryKind kind = is_dir ? EntryKind::DIR : EntryKind::FILE;
            const std::uint64_t size = (op == FsOp::REMOVED) ? 0 : stat_size(full, is_dir);
            const std::uint64_t mtime = (op == FsOp::REMOVED) ? 0 : stat_mtime(full);
            cb_(op, kind, vpath, size, mtime);
        }
    }
}

#else  // non-Linux: inert stub

void FsWatcher::start() {
    log_warn("filesystem watch not supported on this platform; live events disabled");
    active_ = false;
}
void FsWatcher::stop() {
    if (thread_.joinable()) { stop_.store(true); thread_.join(); }
}
void FsWatcher::run() {}

#endif

} // namespace fileshare::v2
