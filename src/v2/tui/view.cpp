#include "fileshare/v2/tui/view.hpp"

#include <algorithm>

#include <ftxui/screen/color.hpp>

using namespace ftxui;

namespace fileshare::v2::tui {

namespace {

// Classic MC-ish palette on a 16-colour terminal.
const Color kPanelBg   = Color::Blue;
const Color kDirFg     = Color::White;
const Color kNewFg     = Color::Yellow;
const Color kResumeFg  = Color::Cyan;
const Color kFileFg    = Color::GrayLight;

Element pad_right(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return text(std::move(s));
}
Element pad_left(std::string s, std::size_t width) {
    if (s.size() < width) s.insert(0, width - s.size(), ' ');
    return text(std::move(s));
}

// One directory row: [flag][name .......][size][date]
Element render_row(const PanelEntry& e, bool cursor, bool active) {
    const bool is_dotdot = (e.name == "..");
    std::string flag = " ";
    Color name_color = kFileFg;
    bool bold = false;
    if (e.is_dir) { name_color = kDirFg; bold = true; }
    if (e.is_new) { name_color = kNewFg; bold = true; flag = "*"; }

    std::string name = e.is_dir && !is_dotdot ? "/" + e.name : e.name;
    const std::string size = e.is_dir ? "<DIR>" : human_size(e.size);
    const std::string date = human_date(e.mtime);

    Element name_el = pad_right(name, 22);
    if (bold) name_el = name_el | ftxui::bold;
    name_el = name_el | color(name_color);

    Element row = hbox({
        text(flag) | color(kNewFg) | ftxui::bold,
        name_el | flex,
        text(" ") ,
        pad_left(size, 7) | color(kFileFg),
        text("  "),
        text(date) | color(Color::GrayDark),
    });

    if (e.marked) row = row | bgcolor(Color::Blue) | color(Color::Yellow) | ftxui::bold;
    if (cursor) {
        row = active ? (row | inverted | focus) : (row | bgcolor(Color::GrayDark));
    }
    return row;
}

std::string link_badge(Link l) {
    switch (l) {
        case Link::CONNECTED:    return "[*]";
        case Link::RECONNECTING: return "[~]";
        case Link::DOWN:         return "[x]";
    }
    return "[?]";
}

Color link_color(Link l) {
    switch (l) {
        case Link::CONNECTED:    return Color::Green;
        case Link::RECONNECTING: return Color::Yellow;
        case Link::DOWN:         return Color::Red;
    }
    return Color::GrayLight;
}

} // namespace

Element render_panel(const Panel& p, bool active, Link link) {
    Elements rows;
    if (p.loading) {
        rows.push_back(text("  loading...") | color(Color::Yellow));
    } else if (p.entries.empty()) {
        rows.push_back(text("  (empty)") | dim);
    } else {
        for (std::size_t i = 0; i < p.entries.size(); ++i) {
            rows.push_back(render_row(p.entries[i], i == p.cursor, active));
        }
    }

    Element header_el;
    if (p.source == Source::REMOTE) {
        header_el = hbox({text(p.title() + " "),
                          text(link_badge(link)) | color(link_color(link)) | ftxui::bold});
    } else {
        header_el = text(p.title());
    }

    std::string footer = std::to_string(p.entries.size()) + " items";
    if (!p.entries.empty()) footer += ", " + human_size(p.total_size());
    if (p.new_count() > 0) footer += "  new:" + std::to_string(p.new_count());
    if (p.marked_count() > 0) footer += "  *" + std::to_string(p.marked_count());

    Element body = vbox(std::move(rows)) | frame | flex;
    Element box = vbox({
        header_el | (active ? (color(Color::White) | ftxui::bold) : color(Color::GrayLight)),
        separator(),
        body,
        separator(),
        text(footer) | color(Color::GrayLight),
    });
    box = box | border;
    box = active ? (box | color(Color::Cyan)) : box;
    return box | flex;
}

namespace {
std::string human_uptime(std::uint64_t secs) {
    const std::uint64_t d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%llud %02lluh %02llum",
                  static_cast<unsigned long long>(d), static_cast<unsigned long long>(h),
                  static_cast<unsigned long long>(m));
    return buf;
}
std::string bps_str(std::uint64_t bps) {
    return bps == 0 ? std::string("unlimited") : human_size(bps) + "/s";
}
Element kv(const std::string& k, const std::string& v) {
    return hbox({text(k) | color(Color::GrayLight) | size(WIDTH, EQUAL, 22),
                 text(v) | color(Color::White) | ftxui::bold});
}
} // namespace

Element render_admin(const AppState& app) {
    const AdminView& a = app.admin();

    auto tab_label = [&](AdminTab t, const char* n, const char* label) {
        const bool on = a.tab == t;
        return hbox({text(n) | color(Color::Black) | bgcolor(on ? Color::Cyan : Color::GrayDark),
                     text(std::string(label) + " ")
                         | (on ? (color(Color::White) | ftxui::bold) : color(Color::GrayLight))
                         | bgcolor(on ? Color::Blue : Color::Black)});
    };
    Element tabs = hbox({tab_label(AdminTab::OVERVIEW, "1", "Overview"),
                         tab_label(AdminTab::CLIENTS, "2", "Clients"),
                         tab_label(AdminTab::SETTINGS, "3", "Settings")});

    Element body;
    if (a.tab == AdminTab::OVERVIEW) {
        const AdminStats& s = a.stats;
        body = vbox({
            kv("Version", s.version + " (proto 2)"),
            kv("Uptime", human_uptime(s.uptime_seconds)),
            kv("Connections", std::to_string(s.active_connections)),
            kv("Active downloads", std::to_string(s.active_downloads)),
            kv("Bytes sent", human_size(s.bytes_sent)),
            kv("Completed downloads", std::to_string(s.completed_downloads)),
            kv("Per-client limit", bps_str(s.per_client_bps)),
            kv("Global limit", bps_str(s.global_bps)),
        });
    } else if (a.tab == AdminTab::CLIENTS) {
        Elements rows;
        rows.push_back(hbox({text("id") | size(WIDTH, EQUAL, 5),
                             text("login") | size(WIDTH, EQUAL, 12),
                             text("ip") | size(WIDTH, EQUAL, 18),
                             text("role") | size(WIDTH, EQUAL, 7),
                             text("doing")}) | color(Color::GrayLight));
        for (std::size_t i = 0; i < a.clients.size(); ++i) {
            const auto& c = a.clients[i];
            std::string doing = c.current_path.empty() ? "(idle)" : ("get " + c.current_path);
            Element row = hbox({
                text(std::to_string(c.session_id)) | size(WIDTH, EQUAL, 5),
                text(c.login) | size(WIDTH, EQUAL, 12),
                text(c.ip) | size(WIDTH, EQUAL, 18),
                text(c.role == Role::ADMIN ? "admin" : "user") | size(WIDTH, EQUAL, 7),
                text(doing),
            });
            if (i == a.cursor) row = row | inverted | focus;
            rows.push_back(row);
        }
        if (a.clients.empty()) rows.push_back(text("(no clients)") | dim);
        body = vbox(std::move(rows)) | frame | flex;
    } else {  // SETTINGS
        Elements rows;
        for (std::size_t i = 0; i < a.settings.size(); ++i) {
            const auto& [key, val, hot] = a.settings[i];
            Element row = hbox({
                text(key) | size(WIDTH, EQUAL, 28) | color(hot ? Color::White : Color::GrayDark),
                text(val) | size(WIDTH, EQUAL, 18) | ftxui::bold,
                text(hot ? "[hot]" : "[restart]") | color(hot ? Color::Green : Color::GrayDark),
            });
            if (i == a.cursor) row = row | inverted | focus;
            rows.push_back(row);
        }
        body = vbox(std::move(rows)) | frame | flex;
    }

    // Context F-bar.
    auto key = [](const char* n, const char* label) {
        return hbox({text(n) | color(Color::Black) | bgcolor(Color::Cyan),
                     text(label) | color(Color::White) | bgcolor(Color::Blue)});
    };
    Elements fkeys;
    if (a.tab == AdminTab::CLIENTS)  fkeys.push_back(key("8", "Kick "));
    if (a.tab == AdminTab::SETTINGS) fkeys.push_back(key("↵", "Edit "));
    fkeys.push_back(key("R", "Refresh "));
    fkeys.push_back(key("9", "Back "));
    Element fbar = hbox(std::move(fkeys));

    return vbox({
        hbox({text(" ADMIN ") | bgcolor(Color::Red) | color(Color::White) | ftxui::bold,
              text(" "), tabs}),
        separator(),
        body | flex,
        separator(),
        fbar,
    }) | border;
}

Element render_commander(const AppState& app, bool admin, const std::string& prompt) {
    const int a = app.active_index();
    Element panels = hbox({
        render_panel(app.panel(0), a == 0, app.link()) | flex,
        render_panel(app.panel(1), a == 1, app.link()) | flex,
    }) | flex;

    // Transfer line.
    Element transfer;
    if (app.progress().has_value()) {
        const auto& pr = *app.progress();
        const float ratio = pr.total ? static_cast<float>(pr.done) / static_cast<float>(pr.total) : 0.f;
        std::uint64_t eta = 0;
        if (pr.bps > 0 && pr.total > pr.done) eta = (pr.total - pr.done) / pr.bps;
        transfer = hbox({
            text(" " + pr.display + " ") | color(Color::White),
            gauge(ratio) | flex | color(Color::Green),
            text(" " + human_size(pr.done) + "/" + human_size(pr.total)),
            text("  " + human_size(pr.bps) + "/s"),
            text("  eta " + human_eta(eta) + " "),
        }) | bgcolor(Color::Black);
    } else {
        transfer = text("") | dim;
    }

    // Operations log (last few lines).
    Elements log_rows;
    const auto& log = app.op_log();
    const std::size_t show = std::min<std::size_t>(log.size(), 4);
    for (std::size_t i = log.size() - show; i < log.size(); ++i) {
        Color c = Color::GrayLight;
        switch (log[i].level) {
            case LogLevel::GOOD:  c = Color::Green; break;
            case LogLevel::WARN:  c = Color::Yellow; break;
            case LogLevel::ERROR: c = Color::Red; break;
            case LogLevel::INFO:  c = Color::GrayLight; break;
        }
        Element line = text(log[i].text) | color(c);
        if (log[i].level == LogLevel::ERROR) line = line | ftxui::bold;
        log_rows.push_back(line);
    }
    Element logbox = vbox(std::move(log_rows));

    // F-bar.
    auto key = [](const char* n, const char* label) {
        return hbox({text(n) | color(Color::Black) | bgcolor(Color::Cyan),
                     text(label) | color(Color::White) | bgcolor(Color::Blue)});
    };
    Elements fkeys = {
        key("1", "Help "), key("3", "View "), key("4", "Info "),
        key("5", "Copy "), key("6", "Mark "),
    };
    if (admin) fkeys.push_back(key("9", "Admin "));
    fkeys.push_back(key("10", "Quit "));
    Element fbar = hbox(std::move(fkeys));

    Element cmdline = hbox({text(prompt) | color(Color::Green) | ftxui::bold,
                            text("_") | blink});

    return vbox({
        panels,
        transfer,
        logbox,
        cmdline,
        fbar,
    });
}

} // namespace fileshare::v2::tui
