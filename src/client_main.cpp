#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "fileshare/checksum.hpp"
#include "fileshare/cli.hpp"
#include "fileshare/client.hpp"

using namespace fileshare;

namespace {

std::string human_size(std::uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream os;
    os << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' ' << units[unit];
    return os.str();
}

void print_progress(std::uint64_t received, std::uint64_t total) {
    if (total > 0) {
        const int pct = static_cast<int>((received * 100) / total);
        std::cout << "\r  " << pct << "% (" << human_size(received) << " / " << human_size(total)
                  << ")        " << std::flush;
    } else {
        std::cout << "\r  " << human_size(received) << " received        " << std::flush;
    }
}

int run_download(Client& client, const std::string& alias, const std::string& out,
                 std::uint64_t expected_size, bool show_progress) {
    const auto res = client.download(alias, out, expected_size,
                                     show_progress ? print_progress : Client::ProgressFn{});
    if (show_progress) {
        std::cout << "\n";
    }
    if (!res.ok) {
        // Covers server errors, mid-transfer drops, and checksum mismatch. The
        // destination file is left untouched on any failure.
        std::cerr << "download failed: " << res.error << "\n";
        return 1;
    }
    std::cout << "done: " << human_size(res.bytes) << " -> " << out << ", checksum OK\n";
    return 0;
}

void print_help() {
    std::cout << "commands:\n"
                 "  connect <host> <port>        connect to a server\n"
                 "  list                          show available files\n"
                 "  download <alias> [as <path>]  download a file\n"
                 "  disconnect                    close the connection\n"
                 "  help                          this help\n"
                 "  quit                          exit\n";
}

// Interactive REPL. Returns process exit code.
int repl() {
    Client client;
    std::vector<ListEntry> last_list;
    print_help();

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        try {
            if (cmd.empty()) {
                continue;
            } else if (cmd == "connect") {
                std::string host;
                int port = 0;
                in >> host >> port;
                if (host.empty() || port <= 0 || port > 65535) {
                    std::cerr << "usage: connect <host> <port>\n";
                    continue;
                }
                client.connect(host, static_cast<std::uint16_t>(port));
                std::cout << "connected to " << host << ":" << port << "\n";
            } else if (cmd == "list") {
                if (!client.connected()) {
                    std::cerr << "not connected\n";
                    continue;
                }
                last_list = client.request_list();
                std::cout << last_list.size() << " file(s):\n";
                std::size_t idx = 1;
                for (const auto& e : last_list) {
                    std::cout << "  " << idx++ << ") " << e.alias << "   " << human_size(e.size)
                              << "   " << to_hex(e.checksum).substr(0, 8) << "\n";
                }
            } else if (cmd == "download") {
                if (!client.connected()) {
                    std::cerr << "not connected\n";
                    continue;
                }
                std::string alias;
                in >> alias;
                if (alias.empty()) {
                    std::cerr << "usage: download <alias> [as <path>]\n";
                    continue;
                }
                std::string kw;
                std::string out = alias;
                if (in >> kw) {
                    if (kw == "as") {
                        in >> out;
                    } else {
                        out = kw;
                    }
                }
                std::uint64_t expected = 0;
                for (const auto& e : last_list) {
                    if (e.alias == alias) {
                        expected = e.size;
                        break;
                    }
                }
                run_download(client, alias, out, expected, /*show_progress=*/true);
            } else if (cmd == "disconnect") {
                client.disconnect();
                std::cout << "disconnected\n";
            } else if (cmd == "help") {
                print_help();
            } else if (cmd == "quit" || cmd == "exit") {
                break;
            } else {
                std::cerr << "unknown command: " << cmd << " (try 'help')\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // One-shot mode for scripting/tests: --host H --port P --get ALIAS [--out PATH]
    std::string host, alias, out;
    int port = 0;
    bool one_shot = false;
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
            one_shot = true;
        } else if (arg == "--port") {
            const std::string raw = value("--port");
            const auto parsed = parse_port(raw);
            if (!parsed) {
                std::cerr << "invalid --port '" << raw << "' (expected 1-65535)\n";
                return 2;
            }
            port = *parsed;
        } else if (arg == "--get") {
            alias = value("--get");
            one_shot = true;
        } else if (arg == "--out") {
            out = value("--out");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "interactive: fileshare_client\n"
                         "one-shot:    fileshare_client --host H --port P --get ALIAS [--out PATH]\n";
            return 0;
        }
    }

    if (!one_shot) {
        return repl();
    }

    if (host.empty() || port <= 0 || port > 65535 || alias.empty()) {
        std::cerr << "one-shot mode needs --host, --port and --get\n";
        return 2;
    }
    if (out.empty()) {
        out = alias;
    }
    try {
        Client client;
        client.connect(host, static_cast<std::uint16_t>(port));
        std::uint64_t expected = 0;
        for (const auto& e : client.request_list()) {
            if (e.alias == alias) {
                expected = e.size;
                break;
            }
        }
        return run_download(client, alias, out, expected, /*show_progress=*/false);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
