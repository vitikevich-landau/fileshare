// Load-test client for fileshare: sweeps N concurrent downloads and reports
// aggregate throughput, to find where the M2 thread-per-connection server
// saturates (motivating the M4 epoll rewrite). Bytes are discarded after a CRC
// check, so it measures server + network throughput, not client disk speed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fileshare/checksum.hpp"
#include "fileshare/cli.hpp"
#include "fileshare/crc32.hpp"
#include "fileshare/net.hpp"
#include "fileshare/protocol.hpp"

using namespace fileshare;

namespace {

struct ClientResult {
    bool          ok = false;
    std::uint64_t bytes = 0;
};

// One client: connect, download `alias` `repeats` times (discarding bytes but
// verifying the checksum), return total bytes and whether all succeeded.
ClientResult download_discard(const std::string& host, std::uint16_t port,
                              const std::string& alias, int repeats) {
    ClientResult result;
    try {
        net::Socket sock = net::tcp_connect(host, port);
        for (int i = 0; i < repeats; ++i) {
            net::send_all(sock, encode_download_request(DownloadRequest{alias, 0}));
            Crc32 crc;
            std::uint64_t got = 0;
            bool done = false;
            while (!done) {
                const auto frame = net::recv_message(sock);
                if (!frame) {
                    return result; // closed mid-download
                }
                switch (frame->type) {
                    case MessageType::CHUNK_DATA:
                        crc.update(frame->payload.data(), frame->payload.size());
                        got += frame->payload.size();
                        break;
                    case MessageType::DOWNLOAD_DONE: {
                        const Checksum server =
                            parse_download_done(frame->payload.data(), frame->payload.size());
                        if (server != checksum_from_crc32(crc.value())) {
                            return result; // checksum mismatch
                        }
                        done = true;
                        break;
                    }
                    default:
                        return result; // ERROR_MSG or unexpected type
                }
            }
            result.bytes += got;
        }
        result.ok = true;
    } catch (const std::exception&) {
        result.ok = false;
    }
    return result;
}

std::vector<int> parse_int_list(const std::string& csv) {
    std::vector<int> out;
    std::string cur;
    for (const char c : csv) {
        if (c == ',') {
            if (!cur.empty()) {
                out.push_back(std::stoi(cur));
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        out.push_back(std::stoi(cur));
    }
    return out;
}

int run(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::string alias;
    std::string clients_arg = "1,2,4,8,16,32,64";
    std::uint16_t port = 0;
    int repeats = 2;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--host") {
            host = value("--host");
        } else if (arg == "--port") {
            const auto parsed = parse_port(value("--port"));
            if (!parsed) {
                std::cerr << "invalid --port\n";
                return 2;
            }
            port = *parsed;
        } else if (arg == "--alias") {
            alias = value("--alias");
        } else if (arg == "--clients") {
            clients_arg = value("--clients");
        } else if (arg == "--repeats") {
            repeats = std::stoi(value("--repeats"));
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }
    if (port == 0 || alias.empty() || repeats < 1) {
        std::cerr << "usage: fileshare_loadtest --port P --alias A "
                     "[--host H] [--clients 1,2,4,...] [--repeats R]\n";
        return 2;
    }

    net::startup();
    std::cout << "load test: host=" << host << " port=" << port << " alias=" << alias
              << " repeats=" << repeats << "\n\n";
    std::cout << "clients |   total MB |  elapsed s | aggregate MB/s | per-client MB/s |  ok\n";
    std::cout << "--------+------------+------------+----------------+-----------------+------\n";

    for (const int n : parse_int_list(clients_arg)) {
        if (n < 1) {
            continue;
        }
        std::vector<std::thread> threads;
        std::vector<ClientResult> results(static_cast<std::size_t>(n));

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            threads.emplace_back([&, i] {
                results[static_cast<std::size_t>(i)] = download_discard(host, port, alias, repeats);
            });
        }
        for (auto& t : threads) {
            t.join();
        }
        const auto t1 = std::chrono::steady_clock::now();

        const double elapsed = std::chrono::duration<double>(t1 - t0).count();
        std::uint64_t total = 0;
        int ok = 0;
        for (const auto& r : results) {
            total += r.bytes;
            if (r.ok) {
                ++ok;
            }
        }
        const double mb = static_cast<double>(total) / (1024.0 * 1024.0);
        const double aggregate = elapsed > 0.0 ? mb / elapsed : 0.0;
        const double per_client = aggregate / static_cast<double>(n);
        std::printf("%7d | %10.1f | %10.3f | %14.1f | %15.1f | %d/%d\n", n, mb, elapsed, aggregate,
                    per_client, ok, n);
        std::fflush(stdout);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
