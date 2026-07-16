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
