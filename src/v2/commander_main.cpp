// fileshare-commander: the Midnight-Commander-style TUI client.
//
//   fileshare-commander                       # interactive (connect screen)
//   fileshare-commander --host H --port P --login L [--get PATH [--out F]]
//                       [--list PATH] --batch  # scriptable, no TUI
//
// Password comes from --password, the FILESHARE_PASSWORD env var, or an
// interactive prompt.

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <nlohmann/json.hpp>

#include "fileshare/cli.hpp"
#include "fileshare/net.hpp"
#include "fileshare/v2/client.hpp"
#include "fileshare/v2/tui/connection.hpp"
#include "fileshare/v2/tui/model.hpp"
#include "fileshare/v2/tui/view.hpp"

namespace fs = std::filesystem;
using namespace ftxui;
using namespace fileshare::v2;
using namespace fileshare::v2::tui;

namespace {

// --- Profiles ---------------------------------------------------------------
struct Profile {
    std::string   name;
    std::string   host;
    std::uint16_t port = 5555;
    std::string   login;
    std::uint64_t last_seen = 0;
    std::string   downloads_dir;
};

fs::path profiles_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path base = xdg ? fs::path(xdg)
                        : (home ? fs::path(home) / ".config" : fs::path(".config"));
    return base / "fileshare" / "profiles.json";
}

std::vector<Profile> load_profiles() {
    std::vector<Profile> out;
    std::ifstream in(profiles_path());
    if (!in) return out;
    nlohmann::json j;
    try { in >> j; } catch (const std::exception&) { return out; }
    if (!j.is_object() || !j.contains("profiles")) return out;
    for (const auto& pj : j["profiles"]) {
        try {
            Profile p;
            p.name = pj.value("name", "");
            p.host = pj.value("host", "");
            p.port = pj.value("port", static_cast<std::uint16_t>(5555));
            p.login = pj.value("login", "");
            p.last_seen = pj.value("last_seen", static_cast<std::uint64_t>(0));
            p.downloads_dir = pj.value("downloads_dir", "");
            out.push_back(std::move(p));
        } catch (const std::exception&) {}
    }
    return out;
}

void save_profiles(const std::vector<Profile>& profiles) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : profiles) {
        arr.push_back({{"name", p.name}, {"host", p.host}, {"port", p.port},
                       {"login", p.login}, {"last_seen", p.last_seen},
                       {"downloads_dir", p.downloads_dir}});
    }
    std::error_code ec;
    fs::create_directories(profiles_path().parent_path(), ec);
    std::ofstream out(profiles_path());
    if (out) out << nlohmann::json{{"profiles", arr}}.dump(2) << "\n";
}

// --- Connect screen ---------------------------------------------------------
struct ConnectForm {
    std::string host = "127.0.0.1";
    std::string port = "5555";
    std::string login;
    std::string password;
    std::string error;
};

enum class ConnectAction { CONNECT, QUIT };

ConnectAction run_connect_screen(ConnectForm& form, const std::vector<Profile>& profiles) {
    auto screen = ScreenInteractive::Fullscreen();
    ConnectAction action = ConnectAction::QUIT;

    InputOption pw_opt;
    pw_opt.password = true;
    auto in_host  = Input(&form.host, "host");
    auto in_port  = Input(&form.port, "port");
    auto in_login = Input(&form.login, "login");
    auto in_pass  = Input(&form.password, "password", pw_opt);

    auto do_connect = [&] { action = ConnectAction::CONNECT; screen.Exit(); };

    auto connect_btn = Button("Connect", do_connect);
    auto quit_btn = Button("Quit", [&] { action = ConnectAction::QUIT; screen.Exit(); });

    int selected_profile = 0;
    std::vector<std::string> profile_names;
    for (const auto& p : profiles) {
        profile_names.push_back((p.name.empty() ? p.host : p.name) + " (" + p.login + ")");
    }
    auto profile_menu = Menu(&profile_names, &selected_profile);
    auto use_profile = [&] {
        if (selected_profile >= 0 && selected_profile < static_cast<int>(profiles.size())) {
            const Profile& p = profiles[static_cast<std::size_t>(selected_profile)];
            form.host = p.host;
            form.port = std::to_string(p.port);
            form.login = p.login;
        }
    };

    auto layout = Container::Vertical({
        in_host, in_port, in_login, in_pass, profile_menu, connect_btn, quit_btn,
    });

    auto renderer = Renderer(layout, [&] {
        Elements children = {
            text("fileshare commander") | bold | hcenter,
            separator(),
            hbox(text("Host:     "), in_host->Render() | flex),
            hbox(text("Port:     "), in_port->Render() | flex),
            hbox(text("Login:    "), in_login->Render() | flex),
            hbox(text("Password: "), in_pass->Render() | flex),
        };
        if (!profiles.empty()) {
            children.push_back(separator());
            children.push_back(text("Saved (Enter to fill):") | dim);
            children.push_back(profile_menu->Render() | frame | size(HEIGHT, LESS_THAN, 6));
        }
        children.push_back(separator());
        children.push_back(hbox({connect_btn->Render(), text("  "), quit_btn->Render()}) | hcenter);
        if (!form.error.empty()) {
            children.push_back(text(form.error) | color(Color::Red) | bold | hcenter);
        }
        return vbox(children) | border | size(WIDTH, LESS_THAN, 70) | center;
    });

    auto with_keys = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Escape) { action = ConnectAction::QUIT; screen.Exit(); return true; }
        if (e == Event::Return && profile_menu->Focused()) { use_profile(); return true; }
        if (e == Event::Return) { do_connect(); return true; }
        return false;
    });

    screen.Loop(with_keys);
    return action;
}

// --- Prompt string ----------------------------------------------------------
std::string make_prompt(const std::string& login, const std::string& host, const AppState& app) {
    return login + "@" + host + ":" + app.panel(app.active_index()).path + "$ ";
}

// --- Interactive session ----------------------------------------------------
void run_session(Client& client, const std::string& login, const std::string& host,
                 std::uint16_t port, const std::string& password, bool admin, Profile& profile) {
    AppState app;
    app.panel(0).source = Source::LOCAL;
    app.panel(1).source = Source::REMOTE;
    app.panel(1).profile = host;
    app.panel(1).last_seen = profile.last_seen;
    app.load_local(0, fs::current_path().string());
    app.set_link(Link::CONNECTED);

    auto screen = ScreenInteractive::Fullscreen();

    std::mutex inbox_mu;
    std::deque<Result> inbox;
    Connection conn(
        client,
        [&](Result r) {
            { std::lock_guard<std::mutex> lk(inbox_mu); inbox.push_back(std::move(r)); }
            screen.PostEvent(Event::Custom);
        },
        // Reconnect the same Client with the original credentials.
        [&client, host, port, login, password] {
            return client.connect(host, port, login, password).ok;
        });
    conn.set_subscription(SUB_FS | SUB_NOTICE | (admin ? SUB_CONFIG : 0u));
    conn.start();

    // Initial remote listing (SUBSCRIBE is armed by the worker thread).
    app.set_loading(1, true);
    conn.submit(CmdListDir{1, "/"});

    auto drain = [&] {
        std::lock_guard<std::mutex> lk(inbox_mu);
        while (!inbox.empty()) { app.apply(inbox.front()); inbox.pop_front(); }
    };

    auto renderer = Renderer([&] {
        drain();
        return app.admin_open() ? render_admin(app)
                                : render_commander(app, admin, make_prompt(login, host, app));
    });

    auto refresh_active = [&] {
        Panel& p = app.active();
        if (p.source == Source::REMOTE) { app.set_loading(app.active_index(), true);
                                          conn.submit(CmdListDir{app.active_index(), p.path}); }
        else { app.load_local(app.active_index(), p.path); }
    };
    auto refresh_admin = [&] {
        switch (app.admin_tab()) {
            case AdminTab::OVERVIEW: conn.submit(CmdAdminStats{}); break;
            case AdminTab::CLIENTS:  conn.submit(CmdAdminClients{}); break;
            case AdminTab::SETTINGS: conn.submit(CmdAdminGetConfig{}); break;
        }
    };
    auto open_admin_tab = [&](AdminTab t) { app.admin_set_tab(t); refresh_admin(); };

    // Settings-edit modal (shown while `editing`).
    std::string edit_key, edit_value;
    bool editing = false;
    auto edit_input = Input(&edit_value, "new value");
    auto submit_edit = [&] {
        if (!edit_key.empty()) conn.submit(CmdAdminSet{edit_key, edit_value});
        editing = false;
    };
    auto edit_dialog = Container::Vertical({
        edit_input,
        Container::Horizontal({Button("Set", submit_edit), Button("Cancel", [&] { editing = false; })}),
    });
    auto edit_modal = Renderer(edit_dialog, [&] {
        return vbox({
            text("Set " + edit_key) | bold,
            separator(),
            hbox({text("value: "), edit_input->Render() | flex}),
            separator(),
            hbox({edit_dialog->ChildAt(1)->Render()}) | hcenter,
        }) | border | bgcolor(Color::Black) | size(WIDTH, GREATER_THAN, 44);
    });

    auto keys = CatchEvent(renderer, [&](Event e) -> bool {
        // --- Admin panel mode ---
        if (app.admin_open()) {
            if (e == Event::F9 || e == Event::Escape) { app.close_admin(); return true; }
            if (e == Event::Character("1")) { open_admin_tab(AdminTab::OVERVIEW); return true; }
            if (e == Event::Character("2")) { open_admin_tab(AdminTab::CLIENTS); return true; }
            if (e == Event::Character("3")) { open_admin_tab(AdminTab::SETTINGS); return true; }
            if (e == Event::Tab) {
                open_admin_tab(static_cast<AdminTab>((static_cast<int>(app.admin_tab()) + 1) % 3));
                return true;
            }
            if (e == Event::ArrowUp)   { app.admin_move(-1); return true; }
            if (e == Event::ArrowDown) { app.admin_move(1); return true; }
            if (e == Event::Character("\x12")) { refresh_admin(); return true; }  // Ctrl+R
            if (app.admin_tab() == AdminTab::CLIENTS &&
                (e == Event::F8 || e == Event::Character("k"))) {
                if (auto id = app.admin_selected_client()) conn.submit(CmdAdminKick{id});
                return true;
            }
            if (app.admin_tab() == AdminTab::SETTINGS && e == Event::Return) {
                if (auto sel = app.admin_selected_setting(); sel && sel->second) {
                    edit_key = sel->first;
                    edit_value.clear();
                    editing = true;
                }
                return true;
            }
            return true;   // swallow other keys while the panel is open
        }

        // --- Commander mode ---
        if (e == Event::Tab || e == Event::TabReverse) { app.toggle_active(); return true; }
        if (e == Event::ArrowUp)   { app.move_cursor(-1); return true; }
        if (e == Event::ArrowDown) { app.move_cursor(1); return true; }
        if (e == Event::PageUp)    { app.page(-1, 15); return true; }
        if (e == Event::PageDown)  { app.page(1, 15); return true; }
        if (e == Event::Home)      { app.cursor_home(); return true; }
        if (e == Event::End)       { app.cursor_end(); return true; }
        if (e == Event::Return) {
            if (auto cmd = app.enter()) conn.submit(*cmd);
            return true;
        }
        if (e == Event::Character(" ")) { app.toggle_mark_current(); return true; }
        if (e == Event::F5) {
            if (auto cmd = app.make_download()) conn.submit(*cmd);
            return true;
        }
        if (e == Event::F9 && admin) {   // open the admin panel
            app.open_admin();
            conn.submit(CmdAdminStats{});
            conn.submit(CmdAdminGetConfig{});
            conn.submit(CmdAdminClients{});
            return true;
        }
        if (e == Event::Character("*")) { app.invert_marks(); return true; }
        if (e == Event::Character("\x12")) { refresh_active(); return true; }  // Ctrl+R
        if (e == Event::F10 || e == Event::Escape) { screen.Exit(); return true; }
        return false;
    });

    // The edit modal overlays the main UI while `editing`.
    auto root = Modal(keys, edit_modal, &editing);
    screen.Loop(root);
    conn.stop();

    // Remember when we last saw the remote root, for the "new" highlight.
    profile.last_seen = static_cast<std::uint64_t>(std::time(nullptr));
}

// --- Batch mode -------------------------------------------------------------
int run_batch(const std::string& host, std::uint16_t port, const std::string& login,
              const std::string& password, const std::optional<std::string>& get_path,
              const std::optional<std::string>& out_path, const std::optional<std::string>& list_path) {
    Client client;
    const auto cr = client.connect(host, port, login, password);
    if (!cr.ok) {
        std::cerr << "connect failed: " << cr.error << "\n";
        return 1;
    }
    try {
        if (list_path) {
            for (const auto& e : client.list_dir(*list_path)) {
                std::cout << (e.kind == EntryKind::DIR ? "d " : "- ")
                          << e.size << "\t" << e.name << "\n";
            }
        }
        if (get_path) {
            const std::string dst = out_path.value_or(fs::path(*get_path).filename().string());
            const auto r = client.download(*get_path, dst, [](std::uint64_t d, std::uint64_t t) {
                if (t) std::cerr << "\r" << (100 * d / t) << "%   " << std::flush;
            });
            std::cerr << "\n";
            if (!r.ok) { std::cerr << "download failed: " << r.error << "\n"; return 1; }
            std::cout << "done: " << dst << (r.checksum_ok ? " (checksum OK)" : " (unverified)") << "\n";
        }
    } catch (const RemoteError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

std::string prompt_password() {
    std::cerr << "password: " << std::flush;
    std::string pw;
    std::getline(std::cin, pw);
    return pw;
}

} // namespace

int main(int argc, char** argv) {
    std::string host, login, password, port_s = "5555";
    std::optional<std::string> get_path, out_path, list_path;
    bool batch = false, have_password = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* n) -> std::string {
            if (i + 1 >= argc) { std::cerr << n << " needs a value\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--host") host = next("--host");
        else if (a == "--port") port_s = next("--port");
        else if (a == "--login") login = next("--login");
        else if (a == "--password") { password = next("--password"); have_password = true; }
        else if (a == "--get") get_path = next("--get");
        else if (a == "--out") out_path = next("--out");
        else if (a == "--list") list_path = next("--list");
        else if (a == "--batch") batch = true;
        else if (a == "-h" || a == "--help") {
            std::cout << "fileshare-commander [--host H --port P --login L [--batch --get PATH|--list PATH]]\n";
            return 0;
        } else { std::cerr << "unknown argument: " << a << "\n"; return 2; }
    }

    fileshare::net::startup();

    if (batch) {
        const auto port = fileshare::parse_port(port_s);
        if (!port) { std::cerr << "invalid --port\n"; return 2; }
        if (!have_password) {
            if (const char* env = std::getenv("FILESHARE_PASSWORD")) password = env;
            else password = prompt_password();
        }
        return run_batch(host, *port, login, password, get_path, out_path, list_path);
    }

    // Interactive: connect screen -> session.
    std::vector<Profile> profiles = load_profiles();
    ConnectForm form;
    if (!host.empty()) form.host = host;
    if (!login.empty()) form.login = login;
    form.port = port_s;

    for (;;) {
        const ConnectAction action = run_connect_screen(form, profiles);
        if (action == ConnectAction::QUIT) return 0;

        const auto port = fileshare::parse_port(form.port);
        if (!port) { form.error = "invalid port"; continue; }

        Client client;
        const auto cr = client.connect(form.host, *port, form.login, form.password);
        if (!cr.ok) { form.error = "connect failed: " + cr.error; continue; }
        form.error.clear();

        // Find or create a profile for this host.
        Profile* profile = nullptr;
        for (auto& p : profiles) if (p.host == form.host && p.login == form.login) profile = &p;
        if (!profile) {
            profiles.push_back(Profile{form.host, form.host, *port, form.login, 0, ""});
            profile = &profiles.back();
        }

        run_session(client, form.login, form.host, *port, form.password,
                    cr.role == Role::ADMIN, *profile);
        client.disconnect();
        save_profiles(profiles);
        return 0;   // one session per launch in M9
    }
}
