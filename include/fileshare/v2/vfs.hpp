#pragma once

// Virtual filesystem over a single share-root directory. Replaces v1's flat
// Catalog: the whole tree under share_root is served, nothing is added by hand.
//
// SECURITY: resolve() is the ONE place virtual paths become real paths. It
// normalises the path, rejects "..", and canonicalises to prove the result
// stays inside share_root (so a symlink pointing outside can't be followed).
// Every other method routes through it.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "fileshare/types.hpp"
#include "fileshare/v2/protocol.hpp"

namespace fileshare::v2 {

// Carries a protocol ErrCode so the dispatcher can turn a failure straight into
// an ERROR frame without re-classifying it.
class FsError : public std::runtime_error {
public:
    FsError(ErrCode code, const std::string& what) : std::runtime_error(what), code_(code) {}
    [[nodiscard]] ErrCode code() const noexcept { return code_; }
private:
    ErrCode code_;
};

// Normalise a virtual path to a canonical "/a/b" form (no "..", ".", "//",
// no NUL). "" and "/" both mean the root. Throws FsError(BAD_REQUEST) on a
// path that tries to escape or contains an illegal component. Pure string
// operation -- does not touch the filesystem. Exposed for unit testing.
[[nodiscard]] std::string normalize_vpath(const std::string& vpath);

class Vfs {
public:
    // Canonicalises share_root, creating it if missing. Throws FsError on a
    // path that cannot be created or resolved.
    explicit Vfs(const std::filesystem::path& share_root);

    // Directory listing (dirs first, then files, name-sorted). Throws
    // FsError(NOT_A_DIRECTORY / FILE_NOT_FOUND / ACCESS_DENIED).
    [[nodiscard]] std::vector<DirEntry> list(const std::string& vpath) const;

    // stat one entry. Throws FsError(FILE_NOT_FOUND / ACCESS_DENIED). The
    // returned DirEntry.name is the basename ("/" resolves to "").
    [[nodiscard]] DirEntry stat(const std::string& vpath) const;

    // Resolve to a real path inside share_root. Throws FsError(BAD_REQUEST /
    // ACCESS_DENIED / FILE_NOT_FOUND). `must_exist=false` allows resolving a
    // not-yet-created path (used by upload later) while still enforcing the
    // no-escape rule against the nearest existing ancestor.
    [[nodiscard]] std::filesystem::path resolve(const std::string& vpath,
                                                bool must_exist = true) const;

    // Lazy, cached checksum keyed by (vpath, size, mtime). Recomputes when the
    // file changed on disk. Thread-safe. Throws FsError on a missing/again path.
    [[nodiscard]] ChecksumResponse checksum(const std::string& vpath);

    // Drop a cached checksum (called by the fs watcher on modify/remove).
    void invalidate_checksum(const std::string& vpath);

    // Persist / load the checksum cache to a JSON file so a restart does not
    // rehash terabytes. Best-effort: I/O errors are swallowed (logged by caller).
    void load_cache(const std::filesystem::path& file);
    void save_cache(const std::filesystem::path& file) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    struct CacheEntry {
        std::uint64_t size = 0;
        std::uint64_t mtime = 0;
        std::uint8_t  algo = ALGO_PENDING;
        Checksum      checksum{};
    };

    std::filesystem::path root_;   // canonical share-root

    mutable std::mutex                              cache_mutex_;
    std::unordered_map<std::string, CacheEntry>     cache_;
};

// Convert a filesystem timestamp to unix seconds (portable across libstdc++).
[[nodiscard]] std::uint64_t to_unix_seconds(std::filesystem::file_time_type t);

} // namespace fileshare::v2
