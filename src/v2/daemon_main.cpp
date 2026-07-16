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
#include <optional>
#include <string>
#include <thread>

#if !defined(_WIN32)
#  include <termios.h>
#  include <unistd.h>
#endif

#include "fileshare/cli.hpp"
#include "fileshare/net.hpp"
#include "fileshare/v2/auth.hpp"
#include "fileshare/v2/log.hpp"
#include "fileshare/v2/server.hpp"
#include "fileshare/v2/server_context.hpp"
#include "fileshare/v2/settings.hpp"

using namespace fileshare;

namespace {

std::atomic<v2::ServerContext*> g_ctx{nullptr};

void on_signal(int sig) {
    auto* ctx = g_ctx.load();
    if (sig == SIGHUP) {
        if (ctx) ctx->request_reload();   // atomic flag; serve loop re-reads config
        return;
    }
    if (ctx) {
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
        "                        [--check-config] [--log-level debug|info|warn|error]\n"
        "       fileshare-daemon [--config PATH] --add-user LOGIN [--role user|admin]\n"
        "       fileshare-daemon [--config PATH] --reset-password LOGIN\n";
}

// Read a password from the terminal without echoing it.
std::string read_password_noecho(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string pw;
#if !defined(_WIN32)
    termios oldt{};
    const bool is_tty = ::isatty(STDIN_FILENO) != 0;
    if (is_tty && ::tcgetattr(STDIN_FILENO, &oldt) == 0) {
        termios newt = oldt;
        newt.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        std::getline(std::cin, pw);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::cout << "\n";
    } else {
        std::getline(std::cin, pw);
    }
#else
    std::getline(std::cin, pw);
#endif
    return pw;
}

// --add-user / --reset-password flow. Returns a process exit code.
int manage_user(const v2::Settings& settings, const std::string& login,
                std::optional<v2::Role> role, bool reset) {
    v2::UserDb db = v2::UserDb::load(settings.users_file);

    v2::Role effective_role = role.value_or(v2::Role::USER);
    if (reset) {
        const auto existing = db.find(login);
        if (!existing) {
            std::cerr << "no such user: " << login << "\n";
            return 1;
        }
        effective_role = existing->role;   // keep the role on password reset
    }

    const std::string p1 = read_password_noecho("password: ");
    const std::string p2 = read_password_noecho("repeat:   ");
    if (p1.empty()) { std::cerr << "empty password rejected\n"; return 1; }
    if (p1 != p2)   { std::cerr << "passwords do not match\n"; return 1; }

    db.set(v2::make_user(login, effective_role, p1, settings.auth_pbkdf2_iters));
    try {
        db.save(settings.users_file);
    } catch (const std::exception& e) {
        std::cerr << "could not write " << settings.users_file << ": " << e.what() << "\n";
        return 1;
    }
    std::cout << (reset ? "password updated for " : "user added: ") << login
              << " (" << v2::role_to_string(effective_role) << ") -> "
              << settings.users_file << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "config.json";
    std::optional<std::uint16_t> port_override;
    std::optional<std::string>   share_override;
    std::optional<std::string>   log_override;
    std::optional<std::string>   add_user, reset_user;
    std::string                  role_str = "user";
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
        } else if (a == "--add-user") {
            add_user = next("--add-user");
        } else if (a == "--reset-password") {
            reset_user = next("--reset-password");
        } else if (a == "--role") {
            role_str = next("--role");
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

    // User-management subcommands run instead of serving.
    if (add_user || reset_user) {
        if (add_user && reset_user) {
            std::cerr << "--add-user and --reset-password are mutually exclusive\n";
            return 2;
        }
        std::optional<v2::Role> role = v2::role_from_string(role_str);
        if (add_user && !role) {
            std::cerr << "invalid --role (use 'user' or 'admin')\n";
            return 2;
        }
        return manage_user(settings, add_user ? *add_user : *reset_user, role,
                           /*reset=*/reset_user.has_value());
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
