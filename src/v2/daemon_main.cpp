// fileshare-daemon: non-interactive v2 server process.
//
//   fileshare-daemon [--config PATH] [--port N] [--share-root DIR]
//                    [--check-config] [--log-level LEVEL]
//
// No stdin admin console (v1's is gone): management is over the admin protocol
// and signals. SIGTERM/SIGINT -> graceful shutdown; SIGHUP -> reload flag
// (config hot-reload lands in M11).

#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "fileshare/cli.hpp"
#include "fileshare/net.hpp"
#include "fileshare/v2/log.hpp"
#include "fileshare/v2/server.hpp"
#include "fileshare/v2/server_context.hpp"
#include "fileshare/v2/settings.hpp"

using namespace fileshare;

namespace {

std::atomic<v2::ServerContext*> g_ctx{nullptr};

void on_signal(int sig) {
    if (sig == SIGHUP) {
        return;   // reload hook (M11); ignore for now so it doesn't kill us
    }
    if (auto* ctx = g_ctx.load()) {
        ctx->request_stop();   // single atomic store: safe from a handler
    }
}

void install_signal_handlers() {
#ifndef _WIN32
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
#else
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#endif
}

void usage() {
    std::cerr <<
        "usage: fileshare-daemon [--config PATH] [--port N] [--share-root DIR]\n"
        "                        [--check-config] [--log-level debug|info|warn|error]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "config.json";
    std::optional<std::uint16_t> port_override;
    std::optional<std::string>   share_override;
    std::optional<std::string>   log_override;
    bool check_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { std::cerr << name << " needs a value\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--config") {
            config_path = next("--config");
        } else if (a == "--port") {
            const auto p = parse_port(next("--port"));
            if (!p) { std::cerr << "invalid --port\n"; return 2; }
            port_override = *p;
        } else if (a == "--share-root") {
            share_override = next("--share-root");
        } else if (a == "--log-level") {
            log_override = next("--log-level");
        } else if (a == "--check-config") {
            check_only = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            usage();
            return 2;
        }
    }

    v2::Settings settings;
    try {
        settings = v2::Settings::load(config_path);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << "\n";
        return 1;
    }
    if (port_override)  settings.port = *port_override;
    if (share_override) settings.share_root = *share_override;
    if (log_override)   settings.log_level = *log_override;

    if (const std::string err = settings.validate(); !err.empty()) {
        std::cerr << "invalid config: " << err << "\n";
        return 1;
    }
    if (check_only) {
        std::cout << "config OK (port=" << settings.port
                  << ", share_root=" << settings.share_root << ")\n";
        return 0;
    }

    v2::set_log_level(v2::log_level_from_string(settings.log_level));

    try {
        net::startup();
        v2::ServerContext ctx(settings, config_path);
        g_ctx.store(&ctx);
        install_signal_handlers();

        v2::Server server(ctx);
        const std::uint16_t bound = server.bind(settings.port);
        v2::log_info("fileshare-daemon " + std::string(v2::SERVER_VERSION) +
                     " serving on port " + std::to_string(bound) +
                     ", share_root=" + settings.share_root);
        server.serve(/*grace_seconds=*/30);
        g_ctx.store(nullptr);
    } catch (const v2::FsError& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
