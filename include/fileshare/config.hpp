#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "fileshare/types.hpp"

namespace fileshare {

struct SharedFileEntry {
    std::string   alias;
    std::string   path;
    std::uint64_t size_bytes = 0;
    std::string   checksum_algo = "crc32";
    Checksum      checksum{};
};

struct AddOutcome {
    bool            ok = false;
    std::string     error;   // populated when !ok
    SharedFileEntry entry;   // valid when ok
};

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& what) : std::runtime_error(what) {}
};

// In-memory catalog of shared files, persisted to config.json (§5).
//
// add()/remove() only mutate memory; the caller decides when to save() so
// tests and the admin console stay in control of disk writes.
class Catalog {
public:
    // Load from disk. A missing file yields an empty catalog. Throws
    // ConfigError on malformed JSON or an unsupported version.
    [[nodiscard]] static Catalog load(const std::string& config_path);

    // Serialise to disk as pretty JSON. Throws ConfigError on write failure.
    void save(const std::string& config_path) const;

    // Hash `path`, register it under `alias` (defaults to the file name).
    AddOutcome add(const std::string& path,
                   const std::optional<std::string>& alias = std::nullopt);

    // Remove by alias. Returns true if an entry was removed.
    bool remove(const std::string& alias);

    [[nodiscard]] std::optional<SharedFileEntry> find(const std::string& alias) const;
    [[nodiscard]] const std::vector<SharedFileEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<SharedFileEntry> entries_;
};

} // namespace fileshare
