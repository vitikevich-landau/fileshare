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

Connection::Connection(Client& client, ResultSink sink)
    : client_(client), sink_(std::move(sink)) {}

Connection::~Connection() { stop(); }

void Connection::start() {
    if (started_) return;
    started_ = true;
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

void Connection::run() {
    for (;;) {
        Command cmd;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            cmd = std::move(queue_.front());
            queue_.pop_front();
        }
        handle(cmd);
    }
}

void Connection::handle(const Command& cmd) {
    std::visit([this](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        try {
            if constexpr (std::is_same_v<T, CmdListDir>) {
                const auto entries = client_.list_dir(c.path);
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
                if (!r.ok && !client_.connected()) {
                    sink_(ResDisconnected{r.error});
                }
            }
        } catch (const RemoteError& e) {
            sink_(ResError{e.what()});
        } catch (const net::NetError& e) {
            sink_(ResDisconnected{e.what()});
        } catch (const std::exception& e) {
            sink_(ResError{e.what()});
        }
    }, cmd);
}

} // namespace fileshare::v2::tui
