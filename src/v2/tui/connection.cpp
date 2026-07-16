#include "fileshare/v2/tui/connection.hpp"

#include <chrono>
#include <utility>

namespace fileshare::v2::tui {

std::vector<PanelEntry> to_panel_entries(const std::vector<DirEntry>& src) {
    std::vector<PanelEntry> out;
    out.reserve(src.size());
    for (const auto& e : src) {
        PanelEntry pe;
        pe.name = e.name;
        pe.is_dir = (e.kind == EntryKind::DIR);
        pe.size = e.size;
        pe.mtime = e.mtime;
        out.push_back(std::move(pe));
    }
    return out;
}

namespace {
std::string parent_dir(const std::string& p) {
    if (p.empty() || p == "/") return "/";
    const auto pos = p.rfind('/');
    if (pos == 0 || pos == std::string::npos) return "/";
    return p.substr(0, pos);
}
} // namespace

Connection::Connection(Client& client, ResultSink sink, ReconnectFn reconnect)
    : client_(client), sink_(std::move(sink)), reconnect_(std::move(reconnect)) {}

Connection::~Connection() { stop(); }

void Connection::start() {
    if (started_) return;
    started_ = true;
    install_event_handler();
    worker_ = std::thread([this] { run(); });
}

void Connection::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Connection::submit(Command cmd) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(cmd));
    }
    cv_.notify_all();
}

// Translate server-pushed frames into UI Results. Runs on the worker thread.
void Connection::install_event_handler() {
    client_.set_event_handler([this](const Frame& f) {
        if (f.type == Msg::EVENT_FS) {
            const EventFs e = parse_event_fs(f.payload.data(), f.payload.size());
            const char* verb = e.op == FsOp::CREATED ? "appeared"
                             : e.op == FsOp::REMOVED ? "removed" : "changed";
            std::string line = std::string("* ") + verb + ": " + e.path;
            if (e.op != FsOp::REMOVED) line += " (" + human_size(e.size) + ")";
            sink_(ResInfo{line, e.op != FsOp::REMOVED});
            // If the change is in the directory currently shown, re-list it so
            // the panel updates live (new rows get highlighted via last_seen).
            if (last_list_panel_ >= 0 && parent_dir(e.path) == last_list_path_) {
                submit(CmdListDir{last_list_panel_, last_list_path_});
            }
        } else if (f.type == Msg::EVENT_NOTICE) {
            const EventNotice n = parse_event_notice(f.payload.data(), f.payload.size());
            sink_(ResInfo{"server: " + n.text, false});
        }
        // EVENT_CONFIG handled by the admin panel (M11); PONG ignored.
    });
}

void Connection::run() {
    using clock = std::chrono::steady_clock;
    // Arm event delivery from the worker thread (the only thread that touches
    // the Client), so there is no concurrent access with the UI thread.
    try { client_.subscribe(sub_mask_); } catch (const std::exception&) {}
    auto last_ping = clock::now();
    for (;;) {
        Command cmd;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(100),
                         [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            if (!queue_.empty()) { cmd = std::move(queue_.front()); queue_.pop_front(); have = true; }
        }
        if (have) { handle(cmd); continue; }

        // Idle: pump pushed events, then heartbeat.
        if (!poll_and_pump()) { reconnect_loop(); last_ping = clock::now(); continue; }
        if (clock::now() - last_ping > std::chrono::seconds(20)) {
            try { client_.send_ping(); } catch (const std::exception&) { reconnect_loop(); }
            last_ping = clock::now();
        }
    }
}

bool Connection::poll_and_pump() {
    try {
        for (;;) {
            const auto r = client_.poll_events(0);
            if (r == Client::PollResult::CLOSED) return false;
            if (r == Client::PollResult::NONE) return true;
            // EVENT: keep draining.
        }
    } catch (const std::exception&) {
        return false;
    }
}

void Connection::reconnect_loop() {
    if (link_down_) return;
    link_down_ = true;
    sink_(ResDisconnected{"link dropped"});
    if (!reconnect_) return;

    int backoff = 1;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (cv_.wait_for(lk, std::chrono::seconds(backoff), [this] { return stop_; })) return;
        }
        sink_(ResLink{Link::RECONNECTING});
        bool ok = false;
        try { ok = reconnect_(); } catch (const std::exception&) { ok = false; }
        if (ok) {
            install_event_handler();          // re-arm on the fresh socket
            try { client_.subscribe(sub_mask_); } catch (const std::exception&) {}
            link_down_ = false;
            sink_(ResReconnected{});
            if (last_list_panel_ >= 0) submit(CmdListDir{last_list_panel_, last_list_path_});
            return;
        }
        backoff = std::min(backoff * 2, 30);
    }
}

void Connection::handle(const Command& cmd) {
    std::visit([this](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        try {
            if constexpr (std::is_same_v<T, CmdListDir>) {
                const auto entries = client_.list_dir(c.path);
                last_list_panel_ = c.panel;      // remember for live re-list
                last_list_path_ = c.path;
                sink_(ResListing{c.panel, c.path, to_panel_entries(entries)});
            } else if constexpr (std::is_same_v<T, CmdDownload>) {
                using clock = std::chrono::steady_clock;
                const auto start = clock::now();
                auto progress = [&](std::uint64_t done, std::uint64_t total) {
                    const double secs = std::chrono::duration<double>(clock::now() - start).count();
                    const std::uint64_t bps = secs > 0.001
                        ? static_cast<std::uint64_t>(static_cast<double>(done) / secs) : 0;
                    sink_(ResProgress{c.display, done, total, bps});
                };
                const auto r = client_.download(c.remote, c.local, progress);
                sink_(ResDownloadDone{r.ok, r.checksum_ok, c.display, r.error});
                if (!r.ok && !client_.connected()) reconnect_loop();
            } else if constexpr (std::is_same_v<T, CmdAdminStats>) {
                sink_(ResAdminStats{client_.admin_stats()});
            } else if constexpr (std::is_same_v<T, CmdAdminClients>) {
                sink_(ResAdminClients{client_.admin_list_clients()});
            } else if constexpr (std::is_same_v<T, CmdAdminKick>) {
                const auto res = client_.admin_kick(c.session_id);
                sink_(ResInfo{"kick: " + res.message, res.ok});
                sink_(ResAdminClients{client_.admin_list_clients()});   // refresh list
            } else if constexpr (std::is_same_v<T, CmdAdminSet>) {
                const auto res = client_.admin_set(c.key, c.value);
                sink_(ResAdminSetResult{res.ok, res.message});
                sink_(ResAdminConfig{client_.admin_get_config()});      // refresh values
            } else if constexpr (std::is_same_v<T, CmdAdminGetConfig>) {
                sink_(ResAdminConfig{client_.admin_get_config()});
            }
        } catch (const RemoteError& e) {
            sink_(ResError{e.what()});
        } catch (const net::NetError& e) {
            sink_(ResError{e.what()});
            reconnect_loop();
        } catch (const std::exception& e) {
            sink_(ResError{e.what()});
        }
    }, cmd);
}

} // namespace fileshare::v2::tui
