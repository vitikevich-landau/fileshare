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

    // `client` must already be connected; `sink` is called from the worker
    // thread for every Result.
    Connection(Client& client, ResultSink sink);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void start();
    void stop();                 // idempotent; joins the worker
    void submit(Command cmd);    // enqueue work (returns immediately)

private:
    void run();
    void handle(const Command& cmd);

    Client&    client_;
    ResultSink sink_;
    std::thread worker_;

    std::mutex               mu_;
    std::condition_variable  cv_;
    std::deque<Command>      queue_;
    bool                     stop_ = false;
    bool                     started_ = false;
};

// Convert protocol DirEntry rows into UI PanelEntry rows.
[[nodiscard]] std::vector<PanelEntry> to_panel_entries(const std::vector<DirEntry>& src);

} // namespace fileshare::v2::tui
