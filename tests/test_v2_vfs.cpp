#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "fileshare/v2/vfs.hpp"

namespace fs = std::filesystem;
using namespace fileshare::v2;

namespace {

// A throwaway share-root populated with a small tree, cleaned up on destruction.
class TempTree {
public:
    TempTree() {
        root_ = fs::temp_directory_path() /
                fs::path("fileshare_vfs_test_" + std::to_string(::getpid()) + "_" +
                         std::to_string(counter_++));
        fs::create_directories(root_ / "video");
        fs::create_directories(root_ / "docs");
        write(root_ / "readme.txt", "hello world");
        write(root_ / "video" / "clip.bin", std::string(1000, 'x'));
        write(root_ / "docs" / "a.md", "# a");
    }
    ~TempTree() { std::error_code ec; fs::remove_all(root_, ec); }

    const fs::path& root() const { return root_; }
    void write(const fs::path& p, const std::string& data) {
        std::ofstream(p, std::ios::binary) << data;
    }

private:
    fs::path root_;
    static inline int counter_ = 0;
};

} // namespace

// --- normalize_vpath (pure) -------------------------------------------------
TEST(V2VfsNormalize, RootForms) {
    EXPECT_EQ(normalize_vpath(""), "/");
    EXPECT_EQ(normalize_vpath("/"), "/");
    EXPECT_EQ(normalize_vpath("//"), "/");
}

TEST(V2VfsNormalize, CollapsesAndStrips) {
    EXPECT_EQ(normalize_vpath("/a//b/"), "/a/b");
    EXPECT_EQ(normalize_vpath("a/b"), "/a/b");
    EXPECT_EQ(normalize_vpath("/a/./b"), "/a/b");
}

TEST(V2VfsNormalize, RejectsDotDot) {
    EXPECT_THROW((void)normalize_vpath("/a/../b"), FsError);
    EXPECT_THROW((void)normalize_vpath("../etc/passwd"), FsError);
    EXPECT_THROW((void)normalize_vpath("/.."), FsError);
}

TEST(V2VfsNormalize, RejectsNul) {
    std::string p = "/a";
    p.push_back('\0');
    p += "b";
    EXPECT_THROW((void)normalize_vpath(p), FsError);
}

// --- Listing ----------------------------------------------------------------
TEST(V2Vfs, ListRootDirsFirst) {
    TempTree t;
    Vfs vfs(t.root());
    const auto entries = vfs.list("/");
    ASSERT_EQ(entries.size(), 3u);
    // dirs first, name-sorted: docs, video, then readme.txt
    EXPECT_EQ(entries[0].name, "docs");
    EXPECT_EQ(entries[0].kind, EntryKind::DIR);
    EXPECT_EQ(entries[1].name, "video");
    EXPECT_EQ(entries[1].kind, EntryKind::DIR);
    EXPECT_EQ(entries[2].name, "readme.txt");
    EXPECT_EQ(entries[2].kind, EntryKind::FILE);
    EXPECT_EQ(entries[2].size, 11u);
}

TEST(V2Vfs, ListSubdir) {
    TempTree t;
    Vfs vfs(t.root());
    const auto entries = vfs.list("/video");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "clip.bin");
    EXPECT_EQ(entries[0].size, 1000u);
}

TEST(V2Vfs, ListMissingThrowsNotFound) {
    TempTree t;
    Vfs vfs(t.root());
    try {
        (void)vfs.list("/nope");
        FAIL() << "expected FsError";
    } catch (const FsError& e) {
        EXPECT_EQ(e.code(), ErrCode::FILE_NOT_FOUND);
    }
}

TEST(V2Vfs, ListOnFileThrowsNotADirectory) {
    TempTree t;
    Vfs vfs(t.root());
    try {
        (void)vfs.list("/readme.txt");
        FAIL() << "expected FsError";
    } catch (const FsError& e) {
        EXPECT_EQ(e.code(), ErrCode::NOT_A_DIRECTORY);
    }
}

// --- stat -------------------------------------------------------------------
TEST(V2Vfs, StatFile) {
    TempTree t;
    Vfs vfs(t.root());
    const DirEntry e = vfs.stat("/video/clip.bin");
    EXPECT_EQ(e.name, "clip.bin");
    EXPECT_EQ(e.kind, EntryKind::FILE);
    EXPECT_EQ(e.size, 1000u);
}

TEST(V2Vfs, StatRootHasEmptyName) {
    TempTree t;
    Vfs vfs(t.root());
    const DirEntry e = vfs.stat("/");
    EXPECT_EQ(e.name, "");
    EXPECT_EQ(e.kind, EntryKind::DIR);
}

// --- Security: traversal + symlink escape -----------------------------------
TEST(V2Vfs, ResolveRejectsTraversal) {
    TempTree t;
    Vfs vfs(t.root());
    try {
        (void)vfs.resolve("/../../etc/passwd");
        FAIL() << "expected FsError";
    } catch (const FsError& e) {
        EXPECT_EQ(e.code(), ErrCode::BAD_REQUEST);
    }
}

TEST(V2Vfs, SymlinkEscapeIsBlockedAndHidden) {
    TempTree t;
    // A symlink inside the share pointing at /etc (outside the root).
    std::error_code ec;
    fs::create_symlink("/etc", t.root() / "escape", ec);
    if (ec) GTEST_SKIP() << "symlinks not supported here";

    Vfs vfs(t.root());
    // resolve() must refuse to follow it.
    try {
        (void)vfs.resolve("/escape");
        FAIL() << "expected FsError";
    } catch (const FsError& e) {
        EXPECT_EQ(e.code(), ErrCode::ACCESS_DENIED);
    }
    // and it must not appear in a listing (no existence leak).
    for (const auto& entry : vfs.list("/")) {
        EXPECT_NE(entry.name, "escape");
    }
}

TEST(V2Vfs, ResolveNonexistentLeafStaysInRoot) {
    TempTree t;
    Vfs vfs(t.root());
    // A to-be-created file (upload path) resolves under the root.
    const fs::path r = vfs.resolve("/newfile.bin", /*must_exist=*/false);
    EXPECT_EQ(r.filename().string(), "newfile.bin");
    EXPECT_EQ(r.parent_path(), vfs.root());
}

TEST(V2Vfs, ResolveDanglingSymlinkLeafRejected) {
    TempTree t;
    std::error_code ec;
    // A symlink leaf whose target does not yet exist and points outside the root.
    fs::create_symlink("/outside/does-not-exist", t.root() / "up", ec);
    if (ec) GTEST_SKIP() << "symlinks not supported here";
    Vfs vfs(t.root());
    try {
        (void)vfs.resolve("/up", /*must_exist=*/false);
        FAIL() << "expected FsError";
    } catch (const FsError& e) {
        EXPECT_EQ(e.code(), ErrCode::ACCESS_DENIED);
    }
}

TEST(V2Vfs, InternalSymlinkIsAllowed) {
    TempTree t;
    std::error_code ec;
    fs::create_symlink(t.root() / "readme.txt", t.root() / "link.txt", ec);
    if (ec) GTEST_SKIP() << "symlinks not supported here";
    Vfs vfs(t.root());
    const fs::path r = vfs.resolve("/link.txt");
    EXPECT_EQ(r.filename().string(), "readme.txt");  // canonicalised to the target
}

// --- Confined open (open_beneath) -------------------------------------------
TEST(V2Vfs, OpenBeneathReadsFile) {
    TempTree t;
    Vfs vfs(t.root());
    auto in = vfs.open_beneath("/readme.txt");
    std::string content((std::istreambuf_iterator<char>(*in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello world");
}

TEST(V2Vfs, OpenBeneathRejectsTraversal) {
    TempTree t;
    Vfs vfs(t.root());
    EXPECT_THROW((void)vfs.open_beneath("/../../etc/passwd"), FsError);
}

TEST(V2Vfs, OpenBeneathBlocksSymlinkEscape) {
    TempTree t;
    std::error_code ec;
    fs::create_symlink("/etc/hostname", t.root() / "escape", ec);
    if (ec) GTEST_SKIP() << "symlinks not supported here";
    Vfs vfs(t.root());
    try {
        (void)vfs.open_beneath("/escape");
        FAIL() << "expected FsError";
    } catch (const FsError& e) {
        EXPECT_EQ(e.code(), ErrCode::ACCESS_DENIED);
    }
}

TEST(V2Vfs, OpenBeneathBlocksSymlinkedParentComponent) {
    // The TOCTOU class: an intermediate directory component is a symlink that
    // points outside the root. A path-string reopen would follow it; the
    // confined open must refuse. This is deterministic (no race needed): the
    // symlink is already in place when we open.
    TempTree t;
    std::error_code ec;
    fs::create_directories("/tmp/fileshare_outside_target", ec);
    std::ofstream("/tmp/fileshare_outside_target/secret.txt") << "OUTSIDE";
    fs::create_symlink("/tmp/fileshare_outside_target", t.root() / "via", ec);
    if (ec) GTEST_SKIP() << "symlinks not supported here";
    Vfs vfs(t.root());
    // "/via/secret.txt" traverses an escaping symlink component.
    EXPECT_THROW((void)vfs.open_beneath("/via/secret.txt"), FsError);
    fs::remove_all("/tmp/fileshare_outside_target", ec);
}

// --- Checksum + cache -------------------------------------------------------
TEST(V2Vfs, ChecksumStableAndCached) {
    TempTree t;
    Vfs vfs(t.root());
    const ChecksumResponse a = vfs.checksum("/readme.txt");
    const ChecksumResponse b = vfs.checksum("/readme.txt");
    EXPECT_NE(a.algo, ALGO_PENDING);
    EXPECT_EQ(a.checksum, b.checksum);   // cache hit returns the same value
}

TEST(V2Vfs, ChecksumRecomputesOnChange) {
    TempTree t;
    Vfs vfs(t.root());
    const ChecksumResponse before = vfs.checksum("/readme.txt");
    // Change content + bump mtime so the (size,mtime) cache key differs.
    t.write(t.root() / "readme.txt", "completely different content here");
    fs::last_write_time(t.root() / "readme.txt",
                        fs::file_time_type::clock::now() + std::chrono::seconds(5));
    const ChecksumResponse after = vfs.checksum("/readme.txt");
    EXPECT_NE(before.checksum, after.checksum);
}

TEST(V2Vfs, ChecksumOnDirectoryRejected) {
    TempTree t;
    Vfs vfs(t.root());
    try {
        (void)vfs.checksum("/video");
        FAIL() << "expected FsError";
    } catch (const FsError& e) {
        EXPECT_EQ(e.code(), ErrCode::IS_A_DIRECTORY);
    }
}

TEST(V2Vfs, CachePersistRoundTrip) {
    TempTree t;
    const fs::path cache = t.root() / ".." / ("cache_" + std::to_string(::getpid()) + ".json");
    ChecksumResponse original;
    {
        Vfs vfs(t.root());
        original = vfs.checksum("/readme.txt");
        vfs.save_cache(cache);
    }
    {
        Vfs vfs(t.root());
        vfs.load_cache(cache);
        // Same (size,mtime) -> served from the loaded cache, identical value.
        const ChecksumResponse loaded = vfs.checksum("/readme.txt");
        EXPECT_EQ(loaded.checksum, original.checksum);
    }
    std::error_code ec;
    fs::remove(cache, ec);
}
