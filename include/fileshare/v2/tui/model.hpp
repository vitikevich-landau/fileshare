#pragma once

// UI-side data model for the commander, independent of FTXUI and of the network
// (so it unit-tests as plain C++). The FTXUI view renders an AppState; a
// background connection worker feeds it Results; AppState never does I/O itself.

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "fileshare/v2/protocol.hpp"   // AdminStats, AdminClientInfo

namespace fileshare::v2::tui {

// One row in a panel (local or remote look the same to the view).
struct PanelEntry {
    std::string   name;
    bool          is_dir = false;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;   // unix seconds
    bool          is_new = false;   // mtime past the panel's last-seen threshold
    bool          marked = false;   // Insert/Space multi-select
};

enum class Source { LOCAL, REMOTE };

enum class SortKey { NAME, SIZE, MTIME };

// A single panel: a source, a current directory, its listing and a cursor.
struct Panel {
    Source        source = Source::LOCAL;
    std::string   profile;             // display name for a remote source
    std::string   path = "/";          // current directory (virtual for remote)
    std::vector<PanelEntry> entries;   // includes a synthetic ".." except at root
    std::size_t   cursor = 0;
    std::size_t   scroll = 0;          // top visible row (view maintains it)
    bool          loading = false;     // awaiting a remote listing
    SortKey       sort = SortKey::NAME;
    bool          show_new = true;
    std::uint64_t last_seen = 0;       // threshold for the "new" flag

    [[nodiscard]] const PanelEntry* current() const {
        return cursor < entries.size() ? &entries[cursor] : nullptr;
    }
    [[nodiscard]] std::size_t new_count() const;
    [[nodiscard]] std::uint64_t total_size() const;
    [[nodiscard]] std::size_t marked_count() const;
    [[nodiscard]] std::string title() const;   // e.g. "fs://vps/incoming" or "/home/vit"
};

// A line in the operations log (rendered in a colour by severity).
enum class LogLevel { INFO, GOOD, WARN, ERROR };
struct LogLine { LogLevel level; std::string text; };

// Connection status shown in the remote panel header.
enum class Link { CONNECTED, RECONNECTING, DOWN };

// --- Commands (UI -> worker) and Results (worker -> UI) ---------------------
struct CmdListDir       { int panel; std::string path; };
struct CmdDownload      { std::string remote; std::string local; std::string display; };
struct CmdAdminStats    {};
struct CmdAdminClients  {};
struct CmdAdminKick     { std::uint64_t session_id; };
struct CmdAdminSet      { std::string key; std::string value; };
struct CmdAdminGetConfig{};
using Command = std::variant<CmdListDir, CmdDownload, CmdAdminStats, CmdAdminClients,
                             CmdAdminKick, CmdAdminSet, CmdAdminGetConfig>;

struct ResListing  { int panel; std::string path; std::vector<PanelEntry> entries; };
struct ResError    { std::string message; };
struct ResInfo     { std::string message; bool good = false; };
struct ResProgress { std::string display; std::uint64_t done; std::uint64_t total;
                     std::uint64_t bps; };
struct ResDownloadDone { bool ok; bool checksum_ok; std::string display; std::string error; };
struct ResDisconnected { std::string reason; };
struct ResReconnected  {};                 // link restored after auto-reconnect
struct ResLink         { Link link; };     // link-status transition (e.g. reconnecting)
struct ResAdminStats   { AdminStats stats; };
struct ResAdminClients { std::vector<AdminClientInfo> clients; };
struct ResAdminSetResult { bool ok; std::string message; };
struct ResAdminConfig  { std::string json; };
using Result = std::variant<ResListing, ResError, ResInfo, ResProgress,
                            ResDownloadDone, ResDisconnected, ResReconnected, ResLink,
                            ResAdminStats, ResAdminClients, ResAdminSetResult, ResAdminConfig>;

// --- Admin panel state ------------------------------------------------------
enum class AdminTab { OVERVIEW = 0, CLIENTS = 1, SETTINGS = 2 };

struct AdminView {
    bool        open = false;
    AdminTab    tab = AdminTab::OVERVIEW;
    AdminStats  stats;
    std::vector<AdminClientInfo> clients;
    std::size_t cursor = 0;   // row cursor for the Clients / Settings tabs
    // Settings rows: (key, value, hot?) parsed from ADMIN_CONFIG.
    std::vector<std::tuple<std::string, std::string, bool>> settings;
};

// The whole UI state. Owned and mutated only on the UI thread.
class AppState {
public:
    AppState();

    Panel& active()   { return panels_[active_idx_]; }
    Panel& inactive() { return panels_[active_idx_ ^ 1]; }
    Panel& panel(int i) { return panels_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] const Panel& panel(int i) const { return panels_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] int active_index() const { return active_idx_; }
    void toggle_active() { active_idx_ ^= 1; }

    // --- Navigation (pure; no I/O) ------------------------------------------
    void move_cursor(int delta);
    void cursor_home();
    void cursor_end();
    void page(int direction, std::size_t page_rows);
    void toggle_mark_current();   // marks + advances, like MC
    void invert_marks();

    // Enter the item under the cursor. Returns a Command to run when it needs a
    // remote listing (nullopt for a local cd, which is applied immediately, or
    // when the cursor is on a file).
    std::optional<Command> enter();

    // Apply a freshly-arrived listing to a panel (from the worker).
    void apply_listing(const ResListing& r);
    void set_loading(int panel, bool on);

    // --- Operations log -----------------------------------------------------
    void log(LogLevel lvl, std::string text);
    [[nodiscard]] const std::deque<LogLine>& op_log() const { return log_; }

    // --- Transfer status ----------------------------------------------------
    void set_progress(const ResProgress& p);
    void clear_progress();
    [[nodiscard]] const std::optional<ResProgress>& progress() const { return progress_; }

    // --- Link status --------------------------------------------------------
    void set_link(Link l) { link_ = l; }
    [[nodiscard]] Link link() const { return link_; }

    // --- Admin panel --------------------------------------------------------
    [[nodiscard]] bool admin_open() const { return admin_.open; }
    void open_admin()  { admin_.open = true; }
    void close_admin() { admin_.open = false; }
    [[nodiscard]] AdminTab admin_tab() const { return admin_.tab; }
    void admin_set_tab(AdminTab t) { admin_.tab = t; admin_.cursor = 0; }
    void admin_move(int delta);                     // move cursor in the active tab list
    [[nodiscard]] const AdminView& admin() const { return admin_; }
    // The currently-selected client's session id (0 if none).
    [[nodiscard]] std::uint64_t admin_selected_client() const;
    // The currently-selected settings row (key + hot flag), or nullopt.
    [[nodiscard]] std::optional<std::pair<std::string, bool>> admin_selected_setting() const;

    // Apply any Result from the worker (dispatches to the methods above).
    void apply(const Result& r);

    // A local directory listing (synchronous; local FS is fast). Returns false
    // and logs on error.
    bool load_local(int panel, const std::string& path);

    // Build the F5 download command for the current selection (active must be
    // remote, inactive local). nullopt if not applicable (logs why).
    std::optional<Command> make_download();

private:
    void sort_panel(Panel& p);

    Panel panels_[2];
    int   active_idx_ = 0;
    std::deque<LogLine> log_;
    std::optional<ResProgress> progress_;
    Link  link_ = Link::CONNECTED;
    AdminView admin_;
    static constexpr std::size_t kMaxLog = 500;
};

// Human-readable size, e.g. "1.2M", "4.5G".
[[nodiscard]] std::string human_size(std::uint64_t bytes);
// "HH:MM:SS" or "MM:SS" for an ETA in seconds.
[[nodiscard]] std::string human_eta(std::uint64_t seconds);
// unix seconds -> "dd.mm.yy".
[[nodiscard]] std::string human_date(std::uint64_t unix_seconds);

} // namespace fileshare::v2::tui
