#include "fileshare/config.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "fileshare/checksum.hpp"

namespace fileshare {
namespace {
constexpr int kConfigVersion = 1;
} // namespace

Catalog Catalog::load(const std::string& config_path) {
    Catalog cat;
    std::ifstream in(config_path, std::ios::binary);
    if (!in) {
        return cat;   // missing file -> empty catalog
    }

    // One try/catch translates every nlohmann exception -- parse errors *and*
    // type mismatches on present-but-wrong-typed fields (e.g. "version":"1") --
    // into ConfigError, honouring load()'s documented contract on hostile
    // input. Our own ConfigError throws are not nlohmann::json::exceptions, so
    // they propagate out unwrapped.
    try {
        nlohmann::json j;
        in >> j;

        if (!j.is_object()) {
            throw ConfigError("config root must be a JSON object");
        }
        if (!j.contains("version") || !j.at("version").is_number_integer()) {
            throw ConfigError("config must have an integer 'version' field");
        }
        const int version = j.at("version").get<int>();
        if (version != kConfigVersion) {
            throw ConfigError("unsupported config version: " + std::to_string(version));
        }
        if (!j.contains("shared_files")) {
            return cat;
        }
        const auto& files = j.at("shared_files");
        if (!files.is_array()) {
            throw ConfigError("shared_files must be a JSON array");
        }

        for (const auto& item : files) {
            SharedFileEntry e;
            e.alias         = item.at("alias").get<std::string>();
            e.path          = item.at("path").get<std::string>();
            e.size_bytes    = item.at("size_bytes").get<std::uint64_t>();
            e.checksum_algo = item.value("checksum_algo", std::string("crc32"));
            const auto hex  = item.at("checksum").get<std::string>();
            const auto sum  = checksum_from_hex(hex);
            if (!sum) {
                throw ConfigError("invalid checksum hex for alias: " + e.alias);
            }
            e.checksum = *sum;
            cat.entries_.push_back(std::move(e));
        }
    } catch (const nlohmann::json::exception& e) {
        throw ConfigError(std::string("invalid config JSON: ") + e.what());
    }
    return cat;
}

void Catalog::save(const std::string& config_path) const {
    // Build + serialise inside a try so a nlohmann exception (e.g. dump()
    // throwing type_error on a non-UTF-8 alias/path) surfaces as ConfigError,
    // matching load()'s contract instead of escaping as a raw json exception.
    std::string serialized;
    try {
        nlohmann::json j;
        j["version"] = kConfigVersion;
        j["shared_files"] = nlohmann::json::array();
        for (const auto& e : entries_) {
            nlohmann::json item;
            item["alias"]         = e.alias;
            item["path"]          = e.path;
            item["size_bytes"]    = e.size_bytes;
            item["checksum_algo"] = e.checksum_algo;
            item["checksum"]      = to_hex(e.checksum);
            j["shared_files"].push_back(std::move(item));
        }
        serialized = j.dump(2);
    } catch (const nlohmann::json::exception& e) {
        throw ConfigError(std::string("cannot serialise config: ") + e.what());
    }

    std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw ConfigError("cannot open config for writing: " + config_path);
    }
    out << serialized << '\n';
    if (!out) {
        throw ConfigError("failed to write config: " + config_path);
    }
}

AddOutcome Catalog::add(const std::string& path,
                        const std::optional<std::string>& alias) {
    AddOutcome outcome;

    const std::string effective_alias =
        alias.value_or(std::filesystem::path(path).filename().string());
    if (effective_alias.empty()) {
        outcome.error = "alias is empty";
        return outcome;
    }
    if (effective_alias.size() > MAX_ALIAS_LEN) {
        outcome.error = "alias exceeds " + std::to_string(MAX_ALIAS_LEN) + " bytes";
        return outcome;
    }
    if (find(effective_alias)) {
        outcome.error = "alias already exists: " + effective_alias;
        return outcome;
    }

    const FileDigest digest = compute_file_digest(path);
    if (!digest.ok) {
        outcome.error = digest.error;
        return outcome;
    }

    SharedFileEntry e;
    e.alias         = effective_alias;
    e.path          = path;
    e.size_bytes    = digest.size;
    e.checksum_algo = digest.algo;
    e.checksum      = digest.checksum;

    entries_.push_back(e);
    outcome.ok = true;
    outcome.entry = std::move(e);
    return outcome;
}

bool Catalog::remove(const std::string& alias) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->alias == alias) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

std::optional<SharedFileEntry> Catalog::find(const std::string& alias) const {
    for (const auto& e : entries_) {
        if (e.alias == alias) {
            return e;
        }
    }
    return std::nullopt;
}

} // namespace fileshare
