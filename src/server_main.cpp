#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fileshare/checksum.hpp"
#include "fileshare/cli.hpp"
#include "fileshare/config.hpp"
#include "fileshare/server.hpp"

#ifdef FILESHARE_HAVE_EPOLL
#  include "fileshare/epoll_server.hpp"
#endif

#ifdef _WIN32
#  include <io.h>
#  include <cstdio>
#else
#  include <unistd.h>
#endif

using namespace fileshare;

namespace {
bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}
} // namespace

// On Linux the deployment target is the epoll reactor (M4); elsewhere (Windows
// development) the portable thread-per-connection server is used. Both share the
// same listen / serve_forever / stop / submit_command interface.
#ifdef FILESHARE_HAVE_EPOLL
using ServerImpl = EpollServer;
#else
using ServerImpl = Server;
#endif

namespace {
// SIGINT/SIGTERM (Ctrl+C, `docker stop`) trigger a graceful shutdown. The
// handler does only async-signal-safe work: stop() is a single atomic store.
std::atomic<ServerImpl*> g_server{nullptr};
extern "C" void handle_signal(int /*sig*/) {
    ServerImpl* server = g_server.load();
    if (server != nullptr) {
        server->stop();
    }
}
} // namespace

namespace {

void print_usage() {
    std::cout << "usage: fileshare_server [--port N] [--config PATH] [--add PATH[=ALIAS]]...\n"
                 "  --port N          TCP port to listen on (default 5555)\n"
                 "  --config PATH     config.json to load/save (default ./config.json)\n"
                 "  --add PATH[=ALIAS] register a file for sharing (repeatable)\n";
}

} // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 5555;
    std::string config_path = "config.json";
    std::vector<std::pair<std::string, std::optional<std::string>>> adds;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--port") {
            const std::string raw = value("--port");
            const auto parsed = parse_port(raw);
            if (!parsed) {
                std::cerr << "invalid --port '" << raw << "' (expected 1-65535)\n";
                return 2;
            }
            port = *parsed;
        } else if (arg == "--config") {
            config_path = value("--config");
        } else if (arg == "--add") {
            const std::string spec = value("--add");
            const auto eq = spec.find('=');
            if (eq == std::string::npos) {
                adds.emplace_back(spec, std::nullopt);
            } else {
                adds.emplace_back(spec.substr(0, eq), spec.substr(eq + 1));
            }
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    Catalog catalog;
    try {
        catalog = Catalog::load(config_path);
    } catch (const ConfigError& e) {
        std::cerr << "config load failed: " << e.what() << "\n";
        return 1;
    }

    bool dirty = false;
    for (const auto& [path, alias] : adds) {
        const auto result = catalog.add(path, alias);
        if (!result.ok) {
            std::cerr << "add failed for " << path << ": " << result.error << "\n";
            return 1;
        }
        std::cout << "added: " << result.entry.alias << " (" << result.entry.size_bytes
                  << " bytes)\n";
        dirty = true;
    }
    if (dirty) {
        try {
            catalog.save(config_path);
        } catch (const ConfigError& e) {
            std::cerr << "config save failed: " << e.what() << "\n";
            return 1;
        }
    }

    std::cout << "catalog (" << catalog.size() << " files):\n";
    for (const auto& e : catalog.entries()) {
        std::cout << "  " << e.alias << "  " << e.size_bytes << " bytes  "
                  << to_hex(e.checksum).substr(0, 8) << "\n";
    }

    ServerImpl server(std::move(catalog), config_path);
    std::uint16_t bound = 0;
    try {
        bound = server.listen(port);
    } catch (const net::NetError& e) {
        std::cerr << "listen failed: " << e.what() << "\n";
        return 1;
    }

    std::cout << "serving on port " << bound << " -- admin console ready (type 'help')\n";

    g_server.store(&server);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // Run the admin console on a detached thread and the network engine on the
    // main thread. A signal (handler -> stop()) or a `shutdown` command then
    // unblocks the engine and the process exits, regardless of the console being
    // parked in a blocking getline -- which no signal can reliably interrupt
    // (glibc restarts the read internally on EINTR).
    std::thread console([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            server.submit_command(line);
            std::istringstream tokens(line);
            std::string verb;
            tokens >> verb;
            if (verb == "shutdown") {
                break; // the queued command stops the engine
            }
        }
        // stdin EOF: an interactive Ctrl+D shuts down; a non-TTY EOF (e.g.
        // `docker run -d`) leaves the server running until a signal.
        if (stdin_is_tty()) {
            server.stop();
        }
    });
    console.detach();

    try {
        server.serve_forever(); // returns on stop() (signal / shutdown / TTY EOF)
    } catch (const std::exception& e) {
        std::cerr << "engine error: " << e.what() << "\n";
    }

    // serve_forever has already drained and torn everything down. The detached
    // console may still be blocked in getline touching std::cin, so exit now
    // rather than let return-from-main tear std::cin down underneath it.
    std::cout.flush();
    std::_Exit(0);
}
