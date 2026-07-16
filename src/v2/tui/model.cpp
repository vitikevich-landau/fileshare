#include "fileshare/v2/tui/model.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace fileshare::v2::tui {

// --- Path helpers (unified slash semantics; local paths on POSIX are absolute
//     and slash-separated, same as remote vpaths) ------------------------------
namespace {

std::string vjoin(const std::string& path, const std::string& name) {
    if (path.empty() || path == "/") return "/" + name;
    if (path.back() == '/') return path + name;
    return path + "/" + name;
}

std::string vparent(const std::string& path) {
    if (path.empty() || path == "/") return "/";
    const auto pos = path.rfind('/');
    if (pos == 0 || pos == std::string::npos) return "/";
    return path.substr(0, pos);
}

std::uint64_t file_time_to_unix(fs::file_time_type t) {
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    return secs < 0 ? 0 : static_cast<std::uint64_t>(secs);
}

bool is_dotdot(const PanelEntry& e) { return e.name == ".."; }

} // namespace

// --- Formatting -------------------------------------------------------------
std::string human_size(std::uint64_t bytes) {
    static const std::array<const char*, 5> unit{"B", "K", "M", "G", "T"};
    double v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < unit.size()) { v /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) {
        std::snprintf(buf, sizeof(buf), "%llu%s", static_cast<unsigned long long>(bytes), unit[u]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f%s", v, unit[u]);
    }
    return buf;
}

std::string human_eta(std::uint64_t seconds) {
    const std::uint64_t h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                             static_cast<unsigned long long>(h), static_cast<unsigned long long>(m),
                             static_cast<unsigned long long>(s));
    else std::snprintf(buf, sizeof(buf), "%02llu:%02llu",
                       static_cast<unsigned long long>(m), static_cast<unsigned long long>(s));
    return buf;
}

std::string human_date(std::uint64_t unix_seconds) {
    if (unix_seconds == 0) return "          ";
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%d.%m.%y", &tm);
    return buf;
}

// --- Panel ------------------------------------------------------------------
std::size_t Panel::new_count() const {
    std::size_t n = 0;
    for (const auto& e : entries) if (e.is_new) ++n;
    return n;
}

std::uint64_t Panel::total_size() const {
    std::uint64_t t = 0;
    for (const auto& e : entries) if (!e.is_dir) t += e.size;
    return t;
}

std::size_t Panel::marked_count() const {
    std::size_t n = 0;
    for (const auto& e : entries) if (e.marked) ++n;
    return n;
}

std::string Panel::title() const {
    if (source == Source::REMOTE) return "fs://" + profile + path;
    return path;
}

// --- AppState ---------------------------------------------------------------
AppState::AppState() {
    panels_[0].source = Source::LOCAL;
    panels_[1].source = Source::REMOTE;
    active_idx_ = 0;
}

void AppState::sort_panel(Panel& p) {
    // Keep ".." pinned at the top; sort the rest dirs-first then by key.
    const bool has_dotdot = !p.entries.empty() && is_dotdot(p.entries.front());
    auto begin = p.entries.begin() + (has_dotdot ? 1 : 0);
    std::sort(begin, p.entries.end(), [&](const PanelEntry& a, const PanelEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;   // dirs first
        switch (p.sort) {
            case SortKey::SIZE:  return a.size > b.size;
            case SortKey::MTIME: return a.mtime > b.mtime;
            case SortKey::NAME:  default: return a.name < b.name;
        }
    });
}

void AppState::move_cursor(int delta) {
    Panel& p = active();
    if (p.entries.empty()) { p.cursor = 0; return; }
    long c = static_cast<long>(p.cursor) + delta;
    if (c < 0) c = 0;
    if (c >= static_cast<long>(p.entries.size())) c = static_cast<long>(p.entries.size()) - 1;
    p.cursor = static_cast<std::size_t>(c);
}

void AppState::cursor_home() { active().cursor = 0; }
void AppState::cursor_end() {
    Panel& p = active();
    p.cursor = p.entries.empty() ? 0 : p.entries.size() - 1;
}

void AppState::page(int direction, std::size_t page_rows) {
    move_cursor(direction * static_cast<int>(page_rows == 0 ? 1 : page_rows));
}

void AppState::toggle_mark_current() {
    Panel& p = active();
    PanelEntry* e = p.cursor < p.entries.size() ? &p.entries[p.cursor] : nullptr;
    if (!e || is_dotdot(*e)) return;
    e->marked = !e->marked;
    move_cursor(1);   // advance like Midnight Commander
}

void AppState::invert_marks() {
    for (auto& e : active().entries) {
        if (!is_dotdot(e)) e.marked = !e.marked;
    }
}

std::optional<Command> AppState::enter() {
    Panel& p = active();
    const PanelEntry* e = p.current();
    if (!e) return std::nullopt;

    if (is_dotdot(*e)) {
        const std::string parent = vparent(p.path);
        if (p.source == Source::LOCAL) { load_local(active_idx_, parent); return std::nullopt; }
        set_loading(active_idx_, true);
        return CmdListDir{active_idx_, parent};
    }
    if (e->is_dir) {
        const std::string child = vjoin(p.path, e->name);
        if (p.source == Source::LOCAL) { load_local(active_idx_, child); return std::nullopt; }
        set_loading(active_idx_, true);
        return CmdListDir{active_idx_, child};
    }
    return std::nullopt;   // a file: no-op on Enter (F5 downloads)
}

void AppState::set_loading(int panel, bool on) { panels_[static_cast<std::size_t>(panel)].loading = on; }

void AppState::apply_listing(const ResListing& r) {
    Panel& p = panels_[static_cast<std::size_t>(r.panel)];
    const std::string keep = p.current() ? p.current()->name : std::string{};

    std::vector<PanelEntry> entries;
    if (r.path != "/") entries.push_back(PanelEntry{"..", true, 0, 0, false, false});
    for (auto e : r.entries) {
        e.is_new = (p.show_new && p.last_seen > 0 && e.mtime > p.last_seen);
        entries.push_back(std::move(e));
    }
    p.path = r.path;
    p.entries = std::move(entries);
    p.loading = false;
    sort_panel(p);

    // Preserve the cursor by name when possible (nice across refreshes).
    p.cursor = 0;
    for (std::size_t i = 0; i < p.entries.size(); ++i) {
        if (p.entries[i].name == keep) { p.cursor = i; break; }
    }
    p.scroll = 0;
}

bool AppState::load_local(int panel, const std::string& path) {
    Panel& p = panels_[static_cast<std::size_t>(panel)];
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        log(LogLevel::ERROR, "not a directory: " + path);
        return false;
    }
    const std::string keep = p.current() ? p.current()->name : std::string{};

    std::vector<PanelEntry> entries;
    if (path != "/") entries.push_back(PanelEntry{"..", true, 0, 0, false, false});
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) { log(LogLevel::ERROR, "cannot read: " + path); return false; }
    for (const auto& de : it) {
        PanelEntry e;
        e.name = de.path().filename().string();
        std::error_code fec;
        e.is_dir = de.is_directory(fec);
        e.size = e.is_dir ? 0 : fs::file_size(de.path(), fec);
        if (fec) e.size = 0;
        fec.clear();
        const auto wt = fs::last_write_time(de.path(), fec);
        e.mtime = fec ? 0 : file_time_to_unix(wt);
        e.is_new = (p.show_new && p.last_seen > 0 && e.mtime > p.last_seen);
        entries.push_back(std::move(e));
    }
    p.source = Source::LOCAL;
    p.path = path;
    p.entries = std::move(entries);
    p.loading = false;
    sort_panel(p);
    p.cursor = 0;
    for (std::size_t i = 0; i < p.entries.size(); ++i) {
        if (p.entries[i].name == keep) { p.cursor = i; break; }
    }
    p.scroll = 0;
    return true;
}

void AppState::log(LogLevel lvl, std::string text) {
    log_.push_back(LogLine{lvl, std::move(text)});
    while (log_.size() > kMaxLog) log_.pop_front();
}

void AppState::set_progress(const ResProgress& p) { progress_ = p; }
void AppState::clear_progress() { progress_.reset(); }

std::optional<Command> AppState::make_download() {
    Panel& from = active();
    Panel& to = inactive();
    if (from.source != Source::REMOTE) {
        log(LogLevel::WARN, "download: select a file on the remote panel first");
        return std::nullopt;
    }
    if (to.source != Source::LOCAL) {
        log(LogLevel::WARN, "download: the other panel must be a local directory");
        return std::nullopt;
    }
    const PanelEntry* e = from.current();
    if (!e || is_dotdot(*e) || e->is_dir) {
        log(LogLevel::WARN, "download: select a file (directory download is not supported)");
        return std::nullopt;
    }
    const std::string remote = vjoin(from.path, e->name);
    const std::string local = vjoin(to.path, e->name);
    return CmdDownload{remote, local, e->name};
}

void AppState::apply(const Result& r) {
    std::visit([this](const auto& res) {
        using T = std::decay_t<decltype(res)>;
        if constexpr (std::is_same_v<T, ResListing>) {
            apply_listing(res);
        } else if constexpr (std::is_same_v<T, ResError>) {
            log(LogLevel::ERROR, res.message);
        } else if constexpr (std::is_same_v<T, ResInfo>) {
            log(res.good ? LogLevel::GOOD : LogLevel::INFO, res.message);
        } else if constexpr (std::is_same_v<T, ResProgress>) {
            set_progress(res);
        } else if constexpr (std::is_same_v<T, ResDownloadDone>) {
            clear_progress();
            if (res.ok) {
                log(LogLevel::GOOD, "done: " + res.display +
                        (res.checksum_ok ? " (checksum OK)" : " (checksum not verified)"));
            } else {
                log(LogLevel::ERROR, "failed: " + res.display + " -- " + res.error);
            }
        } else if constexpr (std::is_same_v<T, ResDisconnected>) {
            set_link(Link::DOWN);
            log(LogLevel::ERROR, "connection lost: " + res.reason);
        } else if constexpr (std::is_same_v<T, ResReconnected>) {
            set_link(Link::CONNECTED);
            log(LogLevel::GOOD, "reconnected");
        } else if constexpr (std::is_same_v<T, ResLink>) {
            set_link(res.link);
            if (res.link == Link::RECONNECTING) log(LogLevel::WARN, "reconnecting...");
        }
    }, r);
}

} // namespace fileshare::v2::tui
