#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fileshare/net.hpp"
#include "fileshare/protocol.hpp"

namespace fileshare {

// M1 client: connect to one server, list files, download one file (blocking),
// verifying the checksum the server reports against a locally computed one.
class Client {
public:
    void connect(const std::string& host, std::uint16_t port);
    [[nodiscard]] bool connected() const noexcept { return sock_.valid(); }
    void disconnect() noexcept { sock_.close(); }

    // Query the server's shared-file catalog. Throws ProtocolError / net::NetError.
    [[nodiscard]] std::vector<ListEntry> request_list();

    struct DownloadResult {
        bool          ok = false;
        std::string   error;         // populated when !ok or on checksum mismatch
        std::uint64_t bytes = 0;     // bytes actually received
        bool          checksum_ok = false;
    };

    // received/total bytes; total is 0 when the caller didn't supply expected_size.
    using ProgressFn = std::function<void(std::uint64_t received, std::uint64_t total)>;

    // Download `alias` into `out_path`. expected_size is only used to feed the
    // progress callback; correctness never depends on it.
    DownloadResult download(const std::string& alias, const std::string& out_path,
                            std::uint64_t expected_size = 0, const ProgressFn& progress = {});

private:
    net::Socket sock_;
};

} // namespace fileshare
