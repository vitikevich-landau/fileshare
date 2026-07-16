#include "fileshare/v2/vfs.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

#if defined(__linux__)
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#  include <ext/stdio_filebuf.h>   // __gnu_cxx::stdio_filebuf (GCC/Clang libstdc++)
#endif

#include <nlohmann/json.hpp>

#include "fileshare/checksum.hpp"

namespace fs = std::filesystem;

namespace fileshare::v2 {

// --- Path normalisation -----------------------------------------------------
std::string normalize_vpath(const std::string& vpath) {
    if (vpath.find('\0') != std::string::npos) {
        throw FsError(ErrCode::BAD_REQUEST, "path contains NUL");
    }
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return;               // collapses "//" and a leading "/"
        if (cur == ".") { cur.clear(); return; }
        if (cur == "..") {
            throw FsError(ErrCode::BAD_REQUEST, "path escapes root ('..')");
        }
        parts.push_back(cur);
        cur.clear();
    };
    for (char ch : vpath) {
        if (ch == '/' || ch == '\\') { flush(); }
        else                          { cur.push_back(ch); }
    }
    flush();

    std::string out = "/";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].size() > MAX_NAME_LEN) {
            throw FsError(ErrCode::BAD_REQUEST, "path component too long");
        }
        out += parts[i];
        if (i + 1 < parts.size()) out += '/';
    }
    return out;
}

// --- Timestamp conversion ---------------------------------------------------
std::uint64_t to_unix_seconds(fs::file_time_type t) {
    // libstdc++ lacks clock_cast in some versions; shift by "now" on both clocks.
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    return secs < 0 ? 0 : static_cast<std::uint64_t>(secs);
}

// --- Construction -----------------------------------------------------------
Vfs::Vfs(const fs::path& share_root) {
    std::error_code ec;
    fs::create_directories(share_root, ec);   // ignore "already exists"
    root_ = fs::canonical(share_root, ec);
    if (ec) {
        throw FsError(ErrCode::INTERNAL_ERROR,
                      "cannot resolve share_root: " + share_root.string() + ": " + ec.message());
    }
#if defined(__linux__)
    // Hold an O_PATH handle to the root for openat2-confined opens.
    root_fd_ = ::open(root_.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
#endif
}

Vfs::~Vfs() {
#if defined(__linux__)
    if (root_fd_ >= 0) ::close(root_fd_);
#endif
}

// --- Safe open beneath the root ---------------------------------------------
#if defined(__linux__)
namespace {
// openat2 is not always wrapped by glibc; call the syscall directly.
struct open_how_t { std::uint64_t flags; std::uint64_t mode; std::uint64_t resolve; };
#  ifndef RESOLVE_BENEATH
constexpr std::uint64_t RESOLVE_BENEATH_ = 0x08;
#  else
constexpr std::uint64_t RESOLVE_BENEATH_ = RESOLVE_BENEATH;
#  endif
#  ifndef RESOLVE_NO_MAGICLINKS
constexpr std::uint64_t RESOLVE_NO_MAGICLINKS_ = 0x02;
#  else
constexpr std::uint64_t RESOLVE_NO_MAGICLINKS_ = RESOLVE_NO_MAGICLINKS;
#  endif

int openat2_beneath(int dirfd, const char* path) {
    open_how_t how{};
    how.flags = static_cast<std::uint64_t>(O_RDONLY | O_CLOEXEC);
    how.resolve = RESOLVE_BENEATH_ | RESOLVE_NO_MAGICLINKS_;
    return static_cast<int>(::syscall(SYS_openat2, dirfd, path, &how, sizeof(how)));
}

// istream that owns the filebuf wrapping an fd, so the fd closes with the stream.
struct FdIStream : std::istream {
    __gnu_cxx::stdio_filebuf<char> buf;
    explicit FdIStream(int fd)
        : std::istream(nullptr), buf(fd, std::ios::in | std::ios::binary) { rdbuf(&buf); }
};
} // namespace
#endif

std::unique_ptr<std::istream> Vfs::open_beneath(const std::string& vpath) const {
    const std::string norm = normalize_vpath(vpath);   // rejects '..'/NUL

#if defined(__linux__)
    if (root_fd_ >= 0) {
        const std::string rel = (norm == "/") ? std::string(".") : norm.substr(1);
        const int fd = openat2_beneath(root_fd_, rel.c_str());
        if (fd >= 0) {
            return std::make_unique<FdIStream>(fd);
        }
        const int e = errno;
        if (e == EXDEV || e == ELOOP) {
            throw FsError(ErrCode::ACCESS_DENIED, "path escapes share root");
        }
        if (e == ENOENT || e == ENOTDIR) {
            throw FsError(ErrCode::FILE_NOT_FOUND, "no such path: " + norm);
        }
        if (e != ENOSYS) {   // any other real error (EISDIR handled by reader)
            if (e == EISDIR) {
                // openat2 with O_RDONLY on a dir succeeds on Linux; kept for safety.
                throw FsError(ErrCode::IS_A_DIRECTORY, "is a directory: " + norm);
            }
            throw FsError(ErrCode::INTERNAL_ERROR, "open failed: " + norm);
        }
        // ENOSYS: kernel without openat2 -> fall through to the portable path.
    }
#endif
    // Portable fallback: resolve() (canonical, in-root) then open by path. Has a
    // narrow residual reopen window on non-openat2 platforms; documented.
    const fs::path real = resolve(norm);
    auto s = std::make_unique<std::ifstream>(real, std::ios::binary);
    if (!*s) {
        throw FsError(ErrCode::FILE_NOT_FOUND, "cannot open: " + norm);
    }
    return s;
}

// --- Resolution (the security choke point) ----------------------------------
namespace {
bool is_within(const fs::path& root, const fs::path& p) {
    // Both are canonical here; a component-wise prefix check is exact.
    auto rn = root.lexically_normal();
    auto pn = p.lexically_normal();
    auto it = std::mismatch(rn.begin(), rn.end(), pn.begin(), pn.end());
    return it.first == rn.end();
}
} // namespace

fs::path Vfs::resolve(const std::string& vpath, bool must_exist) const {
    const std::string norm = normalize_vpath(vpath);   // throws BAD_REQUEST
    fs::path candidate = root_;
    if (norm != "/") {
        candidate /= fs::path(norm.substr(1));          // strip leading '/'
    }

    std::error_code ec;
    fs::path canon = fs::canonical(candidate, ec);
    if (ec) {
        if (!must_exist) {
            // Resolve the nearest existing ancestor and enforce no-escape there,
            // so a to-be-created path still can't point outside the root.
            fs::path parent = candidate.parent_path();
            fs::path pcanon = fs::canonical(parent, ec);
            if (ec || !is_within(root_, pcanon)) {
                throw FsError(ErrCode::ACCESS_DENIED, "path escapes share root");
            }
            // The leaf may already exist as a (dangling) symlink whose target is
            // outside the root -- canonical() above failed precisely because the
            // target is absent, so is_within never got to inspect it. Reject any
            // symlink leaf: a create/upload must never follow one out of the root.
            std::error_code lec;
            if (fs::is_symlink(candidate, lec)) {
                throw FsError(ErrCode::ACCESS_DENIED, "refusing to follow a symlink leaf");
            }
            return pcanon / candidate.filename();
        }
        throw FsError(ErrCode::FILE_NOT_FOUND, "no such path: " + norm);
    }
    if (!is_within(root_, canon)) {
        // e.g. a symlink inside the tree pointing outside it.
        throw FsError(ErrCode::ACCESS_DENIED, "path escapes share root");
    }
    return canon;
}

// --- DirEntry from a real path ----------------------------------------------
namespace {
DirEntry make_entry(const std::string& name, const fs::path& real) {
    DirEntry e;
    e.name = name;
    std::error_code ec;
    if (fs::is_directory(real, ec)) {
        e.kind = EntryKind::DIR;
        e.size = 0;
    } else {
        e.kind = EntryKind::FILE;
        const auto sz = fs::file_size(real, ec);
        e.size = ec ? 0 : sz;
    }
    ec.clear();
    const auto wt = fs::last_write_time(real, ec);
    e.mtime = ec ? 0 : to_unix_seconds(wt);
    return e;
}
} // namespace

std::vector<DirEntry> Vfs::list(const std::string& vpath) const {
    const fs::path dir = resolve(vpath);   // throws on invalid/escape/missing
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        throw FsError(ErrCode::NOT_A_DIRECTORY, "not a directory: " + vpath);
    }

    std::vector<DirEntry> out;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        throw FsError(ErrCode::ACCESS_DENIED, "cannot read directory: " + vpath);
    }
    for (const auto& de : it) {
        const fs::path& p = de.path();
        // Drop symlinks that would escape the root: don't leak their existence.
        std::error_code sec;
        if (fs::is_symlink(p, sec)) {
            fs::path canon = fs::canonical(p, sec);
            if (sec || !is_within(root_, canon)) continue;
        }
        out.push_back(make_entry(p.filename().string(), p));
    }

    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.kind != b.kind) return a.kind == EntryKind::DIR;  // dirs first
        return a.name < b.name;
    });
    return out;
}

DirEntry Vfs::stat(const std::string& vpath) const {
    const fs::path real = resolve(vpath);
    const std::string norm = normalize_vpath(vpath);
    const std::string name = (norm == "/") ? std::string{} : real.filename().string();
    return make_entry(name, real);
}

// --- Checksum with cache ----------------------------------------------------
namespace {
std::uint8_t algo_code(const std::string& algo) {
    return algo == "sha256" ? ALGO_SHA256 : ALGO_CRC32;
}
} // namespace

ChecksumResponse Vfs::checksum(const std::string& vpath) {
    const std::string norm = normalize_vpath(vpath);
    const fs::path real = resolve(norm);   // throws FILE_NOT_FOUND / ACCESS_DENIED

    std::error_code ec;
    if (fs::is_directory(real, ec)) {
        throw FsError(ErrCode::IS_A_DIRECTORY, "cannot checksum a directory");
    }
    const std::uint64_t size  = ec ? 0 : fs::file_size(real, ec);
    const std::uint64_t mtime = to_unix_seconds(fs::last_write_time(real, ec));

    {
        std::lock_guard<std::mutex> lk(cache_mutex_);
        auto found = cache_.find(norm);
        if (found != cache_.end() && found->second.size == size &&
            found->second.mtime == mtime && found->second.algo != ALGO_PENDING) {
            ChecksumResponse hit;
            hit.path = norm;
            hit.algo = found->second.algo;
            hit.checksum = found->second.checksum;
            return hit;
        }
    }

    // Compute outside the lock (hashing a big file can take a while). Read the
    // content through the confined open so a hash can never be produced for a
    // file outside the share root (no hash oracle via a swapped symlink).
    auto in = open_beneath(norm);
    FileHasher hasher;
    std::vector<char> hbuf(64 * 1024);
    while (in->read(hbuf.data(), static_cast<std::streamsize>(hbuf.size())) || in->gcount() > 0) {
        hasher.update(reinterpret_cast<const std::uint8_t*>(hbuf.data()),
                      static_cast<std::size_t>(in->gcount()));
    }

    CacheEntry ce;
    ce.size = size;
    ce.mtime = mtime;
    ce.algo = algo_code(FileHasher::algorithm());
    ce.checksum = hasher.value();
    {
        std::lock_guard<std::mutex> lk(cache_mutex_);
        cache_[norm] = ce;
    }

    ChecksumResponse resp;
    resp.path = norm;
    resp.algo = ce.algo;
    resp.checksum = ce.checksum;
    return resp;
}

void Vfs::invalidate_checksum(const std::string& vpath) {
    std::string norm;
    try { norm = normalize_vpath(vpath); } catch (const FsError&) { return; }
    std::lock_guard<std::mutex> lk(cache_mutex_);
    cache_.erase(norm);
}

// --- Cache persistence ------------------------------------------------------
void Vfs::load_cache(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return;
    nlohmann::json j;
    try { in >> j; } catch (const std::exception&) { return; }
    if (!j.is_object()) return;
    std::lock_guard<std::mutex> lk(cache_mutex_);
    for (auto it = j.begin(); it != j.end(); ++it) {
        try {
            const auto& v = it.value();
            CacheEntry ce;
            ce.size  = v.at("size").get<std::uint64_t>();
            ce.mtime = v.at("mtime").get<std::uint64_t>();
            ce.algo  = v.at("algo").get<std::uint8_t>();
            const std::string hex = v.at("checksum").get<std::string>();
            const auto parsed = checksum_from_hex(hex);
            if (!parsed) continue;
            ce.checksum = *parsed;
            cache_[it.key()] = ce;
        } catch (const std::exception&) {
            // skip a malformed entry, keep the rest
        }
    }
}

void Vfs::save_cache(const fs::path& file) const {
    nlohmann::json j = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lk(cache_mutex_);
        for (const auto& [vpath, ce] : cache_) {
            j[vpath] = {
                {"size", ce.size},
                {"mtime", ce.mtime},
                {"algo", ce.algo},
                {"checksum", to_hex(ce.checksum)},
            };
        }
    }
    std::ofstream out(file);
    if (out) out << j.dump(2);
}

} // namespace fileshare::v2
