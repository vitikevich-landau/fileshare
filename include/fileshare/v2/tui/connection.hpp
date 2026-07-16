#pragma once

// Background connection worker: owns the (already-connected) Client and runs all
// network I/O on its own thread, so a slow list or download never freezes the
// UI. The UI submits Commands; the worker posts Results back through a sink
// (which the UI wires to wake its event loop). The Client is touched only by
// this thread.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "fileshare/v2/client.hpp"
#include "fileshare/v2/tui/model.hpp"

namespace fileshare::v2::tui {

class Connection {
public:
    using ResultSink = std::function<void(Result)>;
    // Reconnect + re-authenticate the same Client; returns true on success.
    using ReconnectFn = std::function<bool()>;

    // `client` must already be connected; `sink` is called from the worker
    // thread for every Result. `reconnect` (optional) is invoked with backoff
    // when the link drops.
    Connection(Client& client, ResultSink sink, ReconnectFn reconnect = {});
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void start();
    void stop();                 // idempotent; joins the worker
    void submit(Command cmd);    // enqueue work (returns immediately)

    // Subscription mask to (re-)arm on connect/reconnect.
    void set_subscription(std::uint32_t mask) { sub_mask_ = mask; }

private:
    void run();
    void handle(const Command& cmd);
    void install_event_handler();
    void reconnect_loop();
    [[nodiscard]] bool poll_and_pump();   // drain pending events; false if link dropped

    Client&     client_;
    ResultSink  sink_;
    ReconnectFn reconnect_;
    std::thread worker_;

    std::mutex               mu_;
    std::condition_variable  cv_;
    std::deque<Command>      queue_;
    bool                     stop_ = false;
    bool                     started_ = false;
    bool                     link_down_ = false;

    std::uint32_t sub_mask_ = SUB_FS | SUB_NOTICE;
    int           last_list_panel_ = -1;   // remote panel + its dir, for live re-list
    std::string   last_list_path_;
};

// Convert protocol DirEntry rows into UI PanelEntry rows.
[[nodiscard]] std::vector<PanelEntry> to_panel_entries(const std::vector<DirEntry>& src);

} // namespace fileshare::v2::tui
